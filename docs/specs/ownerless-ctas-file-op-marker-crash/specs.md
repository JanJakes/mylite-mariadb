# Ownerless CTAS File-Op Marker Crash

## Problem Statement

Ownerless file-operation marker coverage now includes `RENAME TABLE`
(`FILE_RENAME`), `CREATE TABLE ... LIKE` (`FILE_CREATE`), `TRUNCATE TABLE`
native truncate/recreate, and `DROP TABLE` (`FILE_DELETE`). CTAS
(`CREATE TABLE ... SELECT`) is the next bounded `FILE_CREATE` class because
MariaDB creates the destination file-per-table tablespace and then populates it
before MyLite reaches ownerless dictionary finish.

Existing ownerless CTAS crash coverage proves no-live recovery keeps the
created table, copied rows, and native files. This slice adds the narrower
durable-boundary assertion: a writer killed after native CTAS completion but
before dictionary finish must leave `concurrency/mylite-concurrency.ckpt`
marked as needing a native file-operation checkpoint before no-live recovery
can drain it.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` starts
  around line `4850`, derives CTAS destination columns, and calls
  `mysql_create_table_no_lock()` around line `4964`.
- `mariadb/sql/sql_insert.cc:select_create::send_eof()` starts around line
  `5377` and completes the CTAS statement after selected rows have been copied.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` starts around
  line `1607` and notes MyLite ownerless file-operation redo evidence.
- `mariadb/storage/innobase/fil/fil0fil.cc:fil_ibd_create()` logs
  `FILE_CREATE` around line `2097`.
- `mariadb/storage/innobase/log/log0recv.cc` parses `FILE_DELETE`,
  `FILE_MODIFY`, `FILE_RENAME`, and `FILE_CREATE` through the same file-op
  recovery switch around line `2803`.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()`
  calls `mark_ownerless_native_file_op_checkpoint_before_dictionary_finish()`
  before the `dictionary-before-finish` hook can stop the process.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  `dictionary-ctas-crash` recovery coverage and checkpoint-marker readers for
  `mylite-concurrency.ckpt`.

## Scope And Non-Goals

In scope:

- Add a hook-only `dictionary-ctas-file-op-marker-crash` selector and CTest.
- Reuse the existing `CREATE TABLE app.ownerless_ctas_crash_copy ENGINE=InnoDB
  AS SELECT ...` crash scenario.
- Assert the native file-op checkpoint marker is set after killing the writer
  at `dictionary-before-finish`, while another ownerless peer is still live.
- Preserve the existing recovery checks for destination metadata, copied rows,
  native `.frm`/`.ibd` files, post-recovery writes, forced `.shm` rebuild, and
  ordinary native reopen.

Out of scope:

- Changing MariaDB redo, InnoDB tablespace, MyLite WAL, or checkpoint formats.
- Claiming full DDL/file-lifecycle recovery for every `FILE_CREATE` path.
- Replacement CTAS marker coverage, `FILE_MODIFY` marker coverage,
  partition/tablespace import-export classes, SQL-level table-lock fault
  injection, or external MariaDB/RQG stress.

## Design

The implementation refactors the existing CTAS crash test into a shared runner
with a boolean marker assertion. The old `dictionary-ctas-crash` selector calls
the runner without the marker assertion, preserving the existing recovery
selector. The new `dictionary-ctas-file-op-marker-crash` selector calls the same
runner with the assertion enabled.

The marker is checked immediately after the writer is killed and before the
held ownerless peer is released. That proves durable prefinish evidence exists
before no-live recovery can checkpoint and clear it.

## Compatibility Impact

No SQL result, C API, PHP API, native storage format, or production behavior
changes. The slice strengthens unsafe-hook crash evidence for ownerless CTAS
and a populated native `FILE_CREATE` class. Broader DDL/file-lifecycle recovery
remains partial.

## DDL Metadata Routing Impact

The dictionary-generation protocol is unchanged. The killed writer still leaves
recovery-sensitive dictionary state that blocks cleanup while a live peer
exists; no-live reopen remains responsible for rebuilding and draining it.

## Directory And Lifecycle Impact

The test exercises existing files inside the MyLite-owned directory:

- `datadir/app/ownerless_ctas_crash_copy.frm`,
- `datadir/app/ownerless_ctas_crash_copy.ibd`,
- `concurrency/mylite-concurrency.ckpt`,
- `concurrency/mylite-concurrency.shm`.

No new directory entries are introduced.

## Native Storage Impact

No native format changes. The test proves MyLite records durable recovery
evidence for MariaDB-native CTAS `FILE_CREATE` before ownerless dictionary
finish.

## Build, Size, License, And Dependencies

No dependency, license, public API, or production binary-size impact. The new
CTest is compiled and registered only when unsafe ownerless test hooks are
enabled.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the new selector: `dictionary-ctas-file-op-marker-crash`.
- Run adjacent selectors: `dictionary-ctas-crash`,
  `dictionary-create-like-file-op-marker-crash`,
  `dictionary-rename-file-op-marker-crash`,
  `dictionary-truncate-file-op-marker-crash`,
  `dictionary-drop-file-op-marker-crash`, and `native-file-op-marker-drain`.
- Run focused hook CTest registration for the file-op marker selectors.
- Build the production `php-embedded-prod` ownerless SQL target to prove unsafe
  hooks compile out.
- Run focused production ownerless CTAS/replay and marker selectors.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after CTAS leaves
  `read_concurrency_native_file_op_checkpoint_needed(database_path)` true
  before the live peer is released.
- Live-peer cleanup remains busy while recovery-sensitive dictionary state is
  present.
- No-live ownerless recovery keeps the CTAS table definition, copied rows, and
  native files present.
- Forced `.shm` rebuild and ordinary native reopen keep the copied table state
  visible.
- The older `dictionary-ctas-crash` selector remains available.

## Risks And Follow-Up

- This covers a focused CTAS `FILE_CREATE` crash boundary, not every
  create-style or replacement-copy DDL combination.
- Replacement CTAS marker coverage, `FILE_MODIFY` marker coverage, broader
  native redo/checkpoint reconciliation, durable DDL lifecycle metadata, and
  external MariaDB/RQG stress remain planned.
