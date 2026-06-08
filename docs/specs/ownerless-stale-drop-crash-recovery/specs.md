# Ownerless Stale Drop Crash Recovery

## Problem

Ownerless recovery has separate evidence for stale-reader page-version WAL
around a completed `DROP TABLE` and for a killed `DROP TABLE` after native file
removal but before ownerless dictionary finish. The remaining bounded gap is
the composition: a stale snapshot retains page-version WAL for a table, then a
`DROP TABLE` writer dies at the dictionary finish boundary after MariaDB has
removed the table files.

This slice proves that MyLite does not replay retained stale page images for
the removed tablespace and does not let a live peer clean recovery-sensitive
dictionary state while a stale reader is still live.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_rm_table_no_locks()` performs the native
  table drop path and calls `ha_delete_table()` before deleting the `.frm`
  definition file.
- `mariadb/sql/handler.cc:ha_delete_table()` dispatches to the storage
  engine's `drop_table` implementation, while `handler::delete_table()` removes
  the handler file extensions.
- `packages/libmylite/src/database.cc:ownerless_begin_dictionary_ddl()` marks
  an ownerless dictionary generation active before MariaDB executes DDL.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()` pauses
  at the unsafe-hook `dictionary-before-finish` boundary before calling
  `mylite_ownerless_dictionary_state_finish_ddl()`.
- `packages/libmylite/src/ownerless_dictionary_state.cc`
  `mylite_ownerless_dictionary_state_finish_ddl()` clears the active
  dictionary owner and advances the shared dictionary generation.
- Existing SQL coverage already proves stale-reader retained WAL skips a
  completed dropped tablespace during no-live replay, and hook coverage already
  proves a killed `DROP TABLE app.ownerless_drop_crash` at
  `dictionary-before-finish` recovers an absent table after no live peer
  remains.

## Design

Add one unsafe-hook ownerless SQL selector:

1. Create `app.ownerless_drop_crash` as an InnoDB file-per-table table with
   large payload rows.
2. Start a peer ownerless repeatable-read transaction with a consistent
   snapshot, publishing a shared page-version pin.
3. Update every row in the target table, forcing retained page-version WAL
   while the stale reader remains live.
4. Start a `DROP TABLE app.ownerless_drop_crash` writer with the existing
   `dictionary-before-finish` hook, wait for the hook, and assert the `.frm`
   and `.ibd` files are already gone.
5. Kill the writer before ownerless dictionary finish.
6. While the stale reader is still live, prove a new ownerless opener returns
   `MYLITE_BUSY` instead of cleaning recovery-sensitive state.
7. Kill the stale reader, then prove no-live ownerless recovery keeps the table
   absent through ownerless reopen, native read/write reopen, and forced
   `.shm` rebuild.

No product code should change unless this composition exposes a bug.

## Compatibility Impact

No public SQL behavior changes. The covered SQL is normal InnoDB update plus
`DROP TABLE` under ownerless read/write opens. The claim is limited to this
hooked crash boundary and does not complete the broader durable DDL
file-lifecycle protocol.

## Directory And Lifecycle Impact

No directory-layout changes. The test exercises existing files under the
MyLite-owned directory:

- `datadir/app/ownerless_drop_crash.frm`,
- `datadir/app/ownerless_drop_crash.ibd`,
- `concurrency/mylite-concurrency.wal`,
- `concurrency/mylite-concurrency.shm`.

The no-live recovery path must leave native table files absent and checkpoint
retained stale-reader WAL without replaying stale images for the dropped
tablespace.

## Native Storage Impact

No native storage format changes. The slice relies on MariaDB-native `DROP
TABLE` file removal and MyLite's existing ownerless recovery bridge.

## Build And Performance Impact

Production builds are unchanged. The new crash selector is only compiled when
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS` is enabled. Verification still
uses production presets for normal build and timing-sensitive checks so CI
timings remain based on optimized binaries.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused `stale-drop-crash-recovery`.
- Run adjacent `dropped-tablespace-replay` and `dictionary-drop-crash`
  selectors.
- Run the new CTest under `ownerless-test-hooks`.
- Build the production embedded ownerless SQL test target with
  `php-embedded-prod` to prove unsafe hooks compile out.
- Run a focused production embedded ownerless/primitives subset.
- Run `format-check-prod` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08:

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test`: passed.
- Focused `stale-drop-crash-recovery` selector with the
  `ownerless-test-hooks` binary: passed.
- Adjacent hook selectors `dropped-tablespace-replay` and
  `dictionary-drop-crash`: passed.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.ownerless-stale-drop-crash-recovery$' --output-on-failure`:
  passed, 1/1 test, 3.87s.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test`: passed with unsafe hooks disabled.
- `ctest --preset php-embedded-prod -R
  'libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-primitives)$'
  --output-on-failure`: passed, 2/2 tests, 2.91s.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Acceptance Criteria

- The stale-reader update retains page-version WAL before the killed drop.
- The writer reaches `dictionary-before-finish` after native `.frm` and `.ibd`
  removal.
- A live stale reader makes peer cleanup return `MYLITE_BUSY`.
- No-live recovery keeps `app.ownerless_drop_crash` absent through ownerless
  reopen, ordinary native reopen, and forced `.shm` rebuild.
- Production builds remain hook-free and optimized.

## Risks And Follow-Up

- This is bounded evidence for one composed file-lifecycle crash state, not a
  full DDL recovery protocol.
- Broader partition, tablespace import/export, storage-option, and randomized
  external DDL/file-lifecycle crash matrices remain planned.
