# Ownerless Dictionary Multi-Rename Crash

## Problem

Ownerless hook coverage kills single-table `RENAME TABLE` writers after
MariaDB moves native files but before MyLite finishes the ownerless dictionary
generation. The remaining DDL/file-lifecycle recovery gap still needs sharper
evidence for multi-object native rename statements, especially the common swap
shape `left TO tmp, right TO left, tmp TO right`.

This slice originally proved that if a writer dies after MariaDB completes that
multi-pair rename but before ownerless dictionary finish, no-live
ownerless/native reopen reconstructs the swapped table state from native
storage. The follow-up
`docs/specs/ownerless-live-rename-list-recovery/specs.md` now upgrades this
selector to prove live-peer recovery for the same explicit schema-qualified
rename-list shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` rejects locked-table and active transaction
    contexts, locks table names, executes the rename list, writes DDL log/binlog
    completion when enabled, and reverts normal-table renames on error.
  - `rename_tables()` treats every two `TABLE_LIST` entries as an old/new pair
    and iterates all pairs in one `RENAME TABLE` statement.
  - `do_rename()` records each normal-table rename in the DDL log and delegates
    to `mysql_rename_table()`.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` builds old/new schema-qualified filenames, calls the
    storage-engine `ha_rename_table()` path, and renames the SQL `.frm`
    metadata file.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` runs the InnoDB DDL transaction, updates
    InnoDB dictionary/table statistics metadata through the native rename path,
    commits, and flushes redo through the DDL commit LSN on success.
- `packages/libmylite/src/database.cc`
  - `ownerless_begin_dictionary_ddl()` arms ownerless dictionary coordination
    before DDL execution.
  - `ownerless_finish_dictionary_ddl()` pauses at the unsafe
    `dictionary-before-finish` hook before publishing the completed ownerless
    dictionary generation.

## Scope And Non-Goals

In scope:

- Hook-build SQL coverage for a same-schema three-pair InnoDB `RENAME TABLE`
  swap.
- Killing the writer at `dictionary-before-finish`, after native rename
  execution and before ownerless dictionary finish.
- Live ownerless recovery while another peer still exists, with native file-op
  marker retention until final no-live drain.
- Ownerless recovery of final metadata, native files, row contents, and swapped
  InnoDB `SPACE` identities.
- Final ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Crash or error injection inside MariaDB's multi-pair rename loop.
- Cross-schema multi-rename crash recovery, which is covered separately by
  `docs/specs/ownerless-cross-schema-multi-rename-crash/specs.md`.
- Foreign-key multi-rename crash recovery.
- Durable DDL file-lifecycle metadata for every native rename/rebuild/drop
  class.
- SQL-level table-lock wait fault injection; prior explored SQL shapes stopped
  before the ownerless table-wait callback.

## Design

- Add a `dictionary-multi-rename-crash` selector to
  `mylite_ownerless_cross_process_sql_test` under
  `MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`.
- Set up two InnoDB tables with distinct rows and note values under `app`.
- Record distinct InnoDB `SPACE` values for the two source names and verify
  final `.frm`/`.ibd` paths exist with no temporary table files.
- Hold a live ownerless peer open, then fork a writer that executes:

  ```sql
  RENAME TABLE
    app.ownerless_multi_rename_crash_left
      TO app.ownerless_multi_rename_crash_tmp,
    app.ownerless_multi_rename_crash_right
      TO app.ownerless_multi_rename_crash_left,
    app.ownerless_multi_rename_crash_tmp
      TO app.ownerless_multi_rename_crash_right
  ```

- Arm `MYLITE_OWNERLESS_TEST_FAULT=dictionary-before-finish` in the writer and
  kill it after the hook signals readiness.
- Verify a new ownerless opener can finish the dead dictionary generation while
  the live peer exists, then verify:
  - `left` now contains the former right rows.
  - `right` now contains the former left rows.
  - the temporary SQL and InnoDB dictionary name is absent.
  - final `SPACE` values are swapped.
  - both final tables accept writes.
- Verify the native file-op marker remains set while the original live peer
  remains open.
- Release the peer, reopen ownerless with no live peers to drain the marker,
  and recheck the final state through ownerless/native reopen before and after
  deleting `concurrency/mylite-concurrency.shm`.

No production code change is intended. The slice exercises existing native DDL
and ownerless dictionary recovery boundaries.

## Compatibility Impact

No SQL syntax or public C API behavior changes. The compatibility matrix moves
the hook-build DDL crash evidence from single-table rename classes to include a
same-schema multi-pair rename swap with live recovery. Overall
DDL/file-lifecycle recovery remains partial until broader durable lifecycle
metadata and randomized/oracle-backed crash coverage exist.

## Directory And Lifecycle Impact

No directory layout changes. The test verifies durable `.frm` and `.ibd` files
remain inside `datadir/app/`, the temporary rename target leaves no durable
file, and volatile shared-memory rebuild does not lose the final swapped native
state.

## Native Storage Impact

No storage-format changes. MariaDB native InnoDB dictionary and file-per-table
rename state remains the authority after the writer dies before ownerless
dictionary finish.

## Embedded Lifecycle And API

No public API changes. The slice covers embedded ownerless open/close behavior
around a killed DDL writer, live peer recovery, final no-live marker drain,
forced `.shm` rebuild, and ordinary native exclusive reopen.

## Build, Size, And Dependencies

No production binary-size, dependency, or license impact. The change adds
hook-build test code and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the
  `ownerless-test-hooks` preset.
- Run the focused `dictionary-multi-rename-crash` selector.
- Run adjacent dictionary rename crash selectors and the hook crash tail.
- Build normal embedded ownerless SQL coverage and run focused multi-rename
  selectors to keep non-hook behavior covered.
- Run relevant CTest subsets, `format-check`, `tidy`, and `git diff --check`.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after a three-pair native
  rename can be recovered by a live ownerless opener while another peer remains
  open.
- After live recovery, `left` and `right` exist, the temporary table is
  absent from SQL and InnoDB metadata, and final native files match the final
  names.
- InnoDB `SPACE` identities are swapped relative to the pre-crash source names.
- Both swapped tables preserve their original rows and accept post-recovery
  writes.
- The native file-op marker remains set while the original live peer remains
  open, then final no-live ownerless recovery drains it.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same final state.

## Risks And Open Questions

- This proves one deterministic same-schema swap shape after native rename
  completion. It does not prove MariaDB DDL-log rollback if a later rename pair
  fails or if the process dies inside the native rename loop.
- Broader DDL/file-lifecycle recovery and external randomized DDL oracles remain
  planned ownerless-concurrency work.
