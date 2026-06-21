# Ownerless Rebuild File-Op Marker Crash

## Problem Statement

Ownerless native file-operation marker coverage now proves crash-visible
checkpoint-needed evidence for representative rename, create, CTAS, truncate,
drop, and replacement-copy table lifecycles. Copy/rebuild-style `ALTER TABLE`
remains a separate class because MariaDB creates an intermediate table,
copies rows, then swaps native files back into the original name before MyLite
reaches ownerless dictionary finish.

Existing ownerless hook coverage kills `ALTER TABLE ... FORCE` and
`ALTER TABLE ... ROW_FORMAT=DYNAMIC` writers after the native rebuild but
before ownerless dictionary finish, then proves no-live recovery preserves the
rebuilt table. This slice adds the narrower durable-boundary assertion: the
killed writer must leave `concurrency/mylite-concurrency.ckpt` marked as
needing a native file-operation checkpoint while another ownerless peer is
still live and before no-live recovery can drain the marker.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_alter_table()` begins around line `10706`.
  Around lines `11337` through `11435`, MariaDB documents and detects
  `ALTER TABLE ... FORCE` as a request to recreate the table.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` routes copy-alter execution
  through the `alter_copy` path around line `11740`. It creates an
  intermediate table around line `11877`, copies rows through
  `copy_data_between_tables()` around line `11943`, and swaps table names
  through `mysql_rename_table()` around lines `12135` through `12167`.
- `mariadb/sql/sql_table.cc:copy_data_between_tables()` starts around line
  `12570` and is the row-copy phase used by copy-style rebuild DDL.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` starts around
  line `1607`; MyLite ownerless hooks make file-operation redo paths relative
  and mark native file-op redo evidence there.
- `mariadb/storage/innobase/fil/fil0fil.cc:fil_rename_tablespace()` logs
  `FILE_RENAME` around line `2015`, and `fil_ibd_create()` logs
  `FILE_CREATE` around line `2097`.
- `mariadb/storage/innobase/log/log0recv.cc` parses `FILE_DELETE`,
  `FILE_MODIFY`, `FILE_RENAME`, and `FILE_CREATE` through the same native
  file-op recovery switch around line `2803`.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()` calls
  `mark_ownerless_native_file_op_checkpoint_before_dictionary_finish()` before
  the `dictionary-before-finish` hook can stop the process.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  `dictionary-force-rebuild-crash` and `dictionary-row-format-crash` recovery
  coverage plus checkpoint marker readers for `mylite-concurrency.ckpt`.

## Scope And Non-Goals

In scope:

- Add hook-only marker selectors and CTests for:
  - `dictionary-force-rebuild-file-op-marker-crash`,
  - `dictionary-row-format-file-op-marker-crash`.
- Reuse the existing rebuild crash SQL shapes:
  - `ALTER TABLE app.ownerless_force_rebuild_crash_base FORCE`,
  - `ALTER TABLE app.ownerless_row_format_base ROW_FORMAT=DYNAMIC`.
- Assert the native file-op checkpoint marker is set after killing the writer
  at `dictionary-before-finish`, while another ownerless peer is still live.
- Preserve the existing recovery checks for rebuilt table metadata, native
  files, retained rows, post-recovery writes, ordinary native reopen, and
  forced `.shm` rebuild.

Out of scope:

- Changing MariaDB redo, InnoDB tablespace, MyLite WAL, or checkpoint formats.
- Claiming every rebuild/replacement, `FILE_MODIFY`, multi-file, partition,
  or tablespace import/export lifecycle shape.
- SQL-level table-lock fault injection or external MariaDB/RQG stress.

## Design

The implementation turns the existing force-rebuild and row-format crash tests
into shared runners with a boolean marker assertion. Existing selectors keep
calling those runners without the marker assertion. New marker selectors call
the same runners with the assertion enabled.

The assertion checks
`read_concurrency_native_file_op_checkpoint_needed(database_path)` immediately
after the writer is killed and before the held ownerless peer is released.
That proves durable prefinish file-operation evidence exists before no-live
recovery has a chance to checkpoint and clear it.

## Compatibility Impact

No SQL result, C API, PHP API, native storage format, production behavior, or
directory layout changes. The slice strengthens unsafe-hook crash evidence for
representative ownerless rebuild DDL. Broader durable DDL/file-lifecycle
recovery remains partial.

## DDL Metadata Routing Impact

The dictionary-generation protocol is unchanged. The killed writer still
leaves recovery-sensitive rebuild dictionary state that blocks cleanup while a
live peer exists; no-live reopen remains responsible for rebuilding and
draining it.

## Directory And Lifecycle Impact

The tests exercise existing files inside the MyLite-owned directory:

- `datadir/app/ownerless_force_rebuild_crash_base.frm`,
- `datadir/app/ownerless_force_rebuild_crash_base.ibd`,
- `datadir/app/ownerless_row_format_base.frm`,
- `datadir/app/ownerless_row_format_base.ibd`,
- `concurrency/mylite-concurrency.ckpt`,
- `concurrency/mylite-concurrency.shm`.

No new directory entries are introduced.

## Native Storage Impact

No native format changes. The test proves MyLite records durable recovery
evidence for representative MariaDB-native copy/rebuild table operations
before ownerless dictionary finish.

## Build, Size, License, And Dependencies

No dependency, license, public API, or production binary-size impact. The new
CTests are compiled and registered only when unsafe ownerless test hooks are
enabled.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the new selectors:
  `dictionary-force-rebuild-file-op-marker-crash` and
  `dictionary-row-format-file-op-marker-crash`.
- Run adjacent selectors:
  `dictionary-force-rebuild-crash`,
  `dictionary-row-format-crash`,
  `dictionary-create-or-replace-like-file-op-marker-crash`,
  `dictionary-create-or-replace-ctas-file-op-marker-crash`, and
  `native-file-op-marker-drain`.
- Run focused hook CTest registration for the file-op marker selectors.
- Build the production `php-embedded-prod` ownerless SQL target and run
  production rebuild/tablespace replay selectors to prove unsafe hooks compile
  out.
- Run focused ownerless DDL stress, `tools/check-ci-production-builds`,
  `format-check-prod`, and `git diff --check`.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Direct hook selectors passed:
  `dictionary-force-rebuild-file-op-marker-crash` and
  `dictionary-row-format-file-op-marker-crash`.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.ownerless-dictionary-(force-rebuild|row-format)-file-op-marker-crash'
  --output-on-failure` passed 2/2.
- Adjacent hook selectors passed:
  `dictionary-force-rebuild-crash`, `dictionary-row-format-crash`, and
  `native-file-op-marker-drain`.
- Focused hook marker CTest group passed 9/9 for rename, create-like, CTAS,
  replacement-copy, force-rebuild, row-format, truncate, and drop marker crash
  selectors.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Production selectors passed: `native-file-op-marker-drain`,
  `force-rebuild-tablespace-replay`, `force-rebuild-ddl`, and
  `row-format-ddl`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed 1/1.

## Acceptance Criteria

- Writers killed at `dictionary-before-finish` after force rebuild and
  row-format rebuild leave
  `read_concurrency_native_file_op_checkpoint_needed(database_path)` true
  before the live peer is released.
- Live-peer cleanup remains busy while recovery-sensitive dictionary state is
  present.
- No-live ownerless recovery keeps the rebuilt table definitions and native
  files.
- Force rebuild preserves table rows, secondary-index metadata, and later
  writes.
- Row-format rebuild preserves dynamic row-format metadata, retained rows, and
  later writes.
- Forced `.shm` rebuild and ordinary native reopen keep rebuilt table state
  visible.
- The older rebuild crash selectors remain available.

## Risks And Follow-Up

- This covers focused representative rebuild crash boundaries, not every
  rebuild/replacement or multi-file DDL combination.
- `FILE_MODIFY` marker coverage, broader native redo/checkpoint
  reconciliation, DDL/file lifecycle recovery, SQL-level table-lock fault
  injection, and external MariaDB/RQG stress remain planned.
