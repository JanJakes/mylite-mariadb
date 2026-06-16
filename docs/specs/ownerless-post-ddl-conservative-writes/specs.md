# Ownerless Post-DDL Conservative Writes

## Goal

Keep ownerless writes correct after local dictionary DDL when another process
has an active page-version snapshot pin. A local writer that renames or
recreates InnoDB file-per-table spaces must not let the following
`INSERT ... VALUES` use the visible fast path until the active external pin is
gone.

## Non-Goals

- Do not broaden SQL-level table-lock fault injection.
- Do not implement full DDL/file lifecycle crash recovery for every DDL class.
- Do not change the ownerless page-version WAL format or InnoDB page-log
  primitive.
- Do not disable the visible fast path for ordinary non-DDL ownerless writes.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit()` writes InnoDB
  page `FIL_PAGE_LSN` values at mini-transaction boundaries; page-version
  publication must therefore preserve the complete page set needed by the
  resulting B-tree state, not only the SQL statement boundary.
- `mariadb/storage/innobase/buf/buf0flu.cc:buf_flush_publish_ownerless_pages_to_lsn()`
  and `buf_flush_publish_ownerless_page_to_lsn()` provide the conservative
  dirty-page publication bridge for native InnoDB pages.
- `packages/libmylite/src/database.cc:ownerless_statement_allows_visible_fast_path()`
  uses `ownerless_peer_dictionary_refresh_requires_conservative_write` to keep
  the next eligible `INSERT ... VALUES` on the conservative bridge after a peer
  dictionary refresh.
- `packages/libmylite/src/database.cc:ownerless_runtime_has_external_page_version_pin()`
  identifies live retained snapshot pins that prevent immediate WAL
  checkpoint/reclaim and require reader-visible page-version evidence.

## Compatibility Impact

This affects ownerless cross-process InnoDB DDL/DML ordering. MariaDB SQL
semantics stay unchanged: a writer that has successfully renamed a table,
created another table at the original name, and inserted into both tables must
read its own final state, and later ownerless/native reopens must keep each
file-per-table tablespace identity distinct.

The performance impact is intentionally bounded. Only ownerless handles that
complete local dictionary DDL while an external page-version pin exists keep
subsequent eligible `INSERT ... VALUES` statements conservative. Once the pin
is gone, the existing fast-path clear rule allows ordinary inserts to resume.

`docs/COMPATIBILITY.md` and the ownerless cross-process concurrency spec need
to mention this post-DDL local-writer guard.

## Design

After successful ownerless dictionary DDL finish and dictionary-cache flush in
both direct and prepared execution paths, check for active external
page-version pins. If one is present, mark the handle's existing
`ownerless_peer_dictionary_refresh_requires_conservative_write` flag.

The fast-path clear rule keeps the flag set while any external page-version pin
is active. This reuses the existing `ownerless_statement_allows_visible_fast_path()`
gate and avoids a new state bit with the same observable semantics:
post-dictionary-change writes must use the conservative native page bridge
until retained readers stop constraining page-version visibility.

## File Lifecycle

The slice does not add files, durable metadata, or directory layout. It changes
when existing ownerless page-version WAL records are produced for native
InnoDB user pages after local DDL, so retained readers and later reopen/replay
paths have complete page evidence for renamed and recreated file-per-table
spaces.

## Embedded Lifecycle And API

No public API changes. The behavior is internal to ownerless read/write handles
and applies equally to direct SQL execution and no-result prepared execution.

## Build, Size, And Dependencies

No new dependencies and no intended binary-size impact. The code change is in
first-party `libmylite` execution policy state, not in upstream-derived
MariaDB source.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run focused SQL case 65,
  `test_ownerless_rename_create_tablespace_replay_keeps_both_spaces`.
- Run the focused ownerless SQL CTest shard or direct case wrapper that
  contains the rename-create tablespace replay case.
- Run formatting and whitespace checks.

## Acceptance Criteria

- The rename/create tablespace replay case observes 13 rows, the inserted
  `id=714` row, and correct primary/secondary index sums before close.
- Ownerless and native reopens, including forced `.shm` rebuild, preserve the
  moved table's original `SPACE` and the recreated original-name table's
  distinct `SPACE`.
- The local diff contains no diagnostic output or abandoned InnoDB hook
  experiment.
- The compatibility matrix and ownerless cross-process spec describe the
  retained-pin post-DDL conservative-write rule.

## Risks And Open Questions

- This is still a conservative policy guard, not proof that every local DDL
  class can safely use visible write fast paths under retained readers.
- Broader DDL/file lifecycle recovery, redo/checkpoint reconciliation, and
  randomized external MariaDB/RQG stress remain separate ownerless completion
  work.
