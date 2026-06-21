# Ownerless Drop File-Op Marker Crash

## Problem Statement

The ownerless prefinish generic file-op marker slice proved that `TRUNCATE
TABLE` leaves the native file-operation checkpoint-needed marker when a writer
is killed after InnoDB emits native `FILE_*` redo and before MyLite finishes
the ownerless dictionary generation. `DROP TABLE` is the next bounded
file-lifecycle class because MariaDB removes the file-per-table tablespace and
emits `FILE_DELETE` redo before MyLite reaches the same dictionary-finish
fault hook.

Existing ownerless `DROP TABLE` crash coverage proves no-live recovery keeps
the table absent. This slice adds the narrower durable-boundary assertion: the
crashed writer must leave `concurrency/mylite-concurrency.ckpt` marked as
needing a native file-operation checkpoint before a live peer is released and
before no-live recovery can drain the marker.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_rm_table_no_locks()` routes `DROP TABLE`
  through the storage-engine delete path before removing SQL-layer table
  metadata.
- `mariadb/storage/innobase/dict/drop.cc` documents that deleted InnoDB files
  require durable `FILE_DELETE` redo evidence.
- `mariadb/storage/innobase/fil/fil0fil.cc:fil_delete_tablespace()` logs
  `FILE_DELETE`, and `mtr_t::log_file_op()` marks MyLite's generic ownerless
  file-operation evidence bit.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()`
  consumes that file-operation evidence and persists the native file-op
  checkpoint-needed marker before the `dictionary-before-finish` hook can stop
  the process.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  `dictionary-drop-crash` recovery coverage and marker readers for
  `mylite-concurrency.ckpt`.

## Scope And Non-Goals

In scope:

- Add a hook-only `dictionary-drop-file-op-marker-crash` selector and CTest.
- Reuse the existing `DROP TABLE app.ownerless_drop_crash` crash scenario.
- Assert the native file-op checkpoint marker is set after killing the writer
  at `dictionary-before-finish`, while another ownerless peer is still live.
- Preserve the existing recovery checks: live-peer cleanup returns busy,
  no-live recovery keeps the table absent, forced `.shm` rebuild keeps the
  table absent, and ordinary native reopen sees the same absence.

Out of scope:

- Changing MariaDB redo, InnoDB tablespace, or MyLite checkpoint formats.
- Adding a durable DDL journal or reconstructing missing DDL-created files.
- Claiming full DDL/file-lifecycle recovery.
- SQL-level table-lock fault injection.
- External MariaDB/RQG stress.

## Design

The implementation refactors the existing drop-crash test into a shared runner
with a boolean marker assertion. The old `dictionary-drop-crash` selector calls
the runner without the marker assertion, preserving the existing all-case
coverage. The new `dictionary-drop-file-op-marker-crash` selector calls the
same runner with the assertion enabled.

The marker is checked immediately after the writer is killed and before the
held ownerless peer is released. That proves durable prefinish evidence exists
before no-live recovery has a chance to checkpoint and clear it.

## Compatibility Impact

No SQL result, C API, PHP API, or native storage behavior changes. The slice
strengthens crash-recovery evidence for ownerless `DROP TABLE` in the unsafe
hook build. Broader DDL/file-lifecycle recovery remains partial.

## DDL Metadata Routing Impact

The dictionary-generation protocol is unchanged. The killed writer still leaves
recovery-sensitive dictionary state that blocks cleanup while a live peer
exists; no-live reopen remains responsible for rebuilding and draining it.

## Directory And Lifecycle Impact

The test exercises existing files inside the MyLite-owned directory:

- `datadir/app/ownerless_drop_crash.frm`,
- `datadir/app/ownerless_drop_crash.ibd`,
- `concurrency/mylite-concurrency.ckpt`,
- `concurrency/mylite-concurrency.shm`.

No new directory entries are introduced.

## Native Storage Impact

No native format changes. The test proves MyLite records durable recovery
evidence for MariaDB-native `FILE_DELETE` before ownerless dictionary finish.

## Build, Size, License, And Dependencies

No dependency, license, public API, or production binary-size impact. The new
CTest is compiled and registered only when unsafe ownerless test hooks are
enabled.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the new selector:
  `dictionary-drop-file-op-marker-crash`.
- Run adjacent selectors:
  `dictionary-drop-crash`, `dictionary-truncate-file-op-marker-crash`,
  `dictionary-rename-file-op-marker-crash`, and `native-file-op-marker-drain`.
- Run the focused hook CTest registration for the drop marker selector.
- Build the production `php-embedded-prod` ownerless SQL target to prove unsafe
  hooks compile out.
- Run focused production ownerless replay/marker selectors.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after `DROP TABLE` leaves
  `read_concurrency_native_file_op_checkpoint_needed(database_path)` true
  before the live peer is released.
- Live-peer cleanup remains busy while recovery-sensitive dictionary state is
  present.
- No-live ownerless recovery keeps `app.ownerless_drop_crash` absent.
- Forced `.shm` rebuild and ordinary native reopen keep the table absent.
- The older `dictionary-drop-crash` selector remains available.

## Risks And Follow-Up

- This covers a focused `DROP TABLE`/`FILE_DELETE` crash boundary, not every
  DDL file-operation class.
- Broader native redo/checkpoint reconciliation, durable DDL lifecycle
  metadata, partition/tablespace import-export classes, and external
  MariaDB/RQG stress remain planned.
