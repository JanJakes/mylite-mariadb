# Ownerless Deferred Latest Checkpoint Coalescing

## Problem

The production embedded performance probe showed that capped visible-fast
multi-row autocommit `INSERT ... VALUES` statements still issue repeated
ownerless latest-only checkpoint updates before the statement publishes its
final page-visible LSN. These updates take the checkpoint byte-range lock and
append new checksummed checkpoint records even though the same statement is
already holding the page-log append session open and will publish a durable
latest-plus-visible checkpoint at the statement boundary.

Skipping all raw-latest checkpoint publication would weaken existing crash
evidence. MyLite fault coverage intentionally distinguishes an older durable
page-visible checkpoint, a newer raw latest LSN, and the final page-visible LSN.
This slice only removes redundant latest-only updates after the first successful
latest checkpoint has been preserved for a visible-fast statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB mini-transaction commit still owns native page-LSN mutation and dirty
  page publication through `mariadb/storage/innobase/mtr/mtr0mtr.cc`. MyLite
  cannot move the durable visibility boundary before those native page changes
  have been published into the ownerless page-version WAL.
- MyLite ownerless redo leave calls publish latest-only checkpoint progress
  through `ownerless_innodb_redo_leave_hook()` and
  `ownerless_persist_redo_checkpoint()` in
  `packages/libmylite/src/database.cc`.
- MyLite ownerless page-visible publication syncs the page-version WAL, updates
  shared redo-visible state, then durably publishes the final latest/visible
  checkpoint through `ownerless_innodb_pages_visible_hook()`.
- Capped visible-fast append batching is selected before SQL execution by
  `ownerless_statement_allows_append_batch_fast_path()` and represented during
  execution by `OwnerlessStatementVisibleFastPathScope`.

## Scope And Non-Goals

In scope:

- Coalesce repeated non-durable latest-only checkpoint updates inside one
  parser-proven implicit/autocommit visible-fast append-batched statement.
- Preserve the first successful latest-only checkpoint update in that statement.
- Preserve the existing final durable latest/visible checkpoint publication.
- Disable this coalescing when unsafe ownerless fault hooks are active.
- Expose a database perf counter and focused SQL coverage proving the
  optimization fires for the capped multi-row fast path.

Out of scope:

- Cross-process checkpoint group commit.
- Coalescing durable page-visible checkpoint writes.
- Changing the checkpoint LSN record format.
- Replacing native redo/checkpoint reconciliation or DDL/file lifecycle
  recovery.
- Extending the parser-proven fast path beyond capped `INSERT ... VALUES`.
- Coalescing latest-only checkpoint updates inside explicit transactions; those
  publish a durable visible boundary at transaction commit and need separate
  proof.

## Design

`OwnerlessStatementVisibleFastPathScope` now tracks whether the current
statement is deferring page-log append-session release, whether that statement
started outside an explicit transaction, and whether a latest-only checkpoint
has already been successfully preserved for that statement. The flags are
thread-local and restored when the statement scope exits, matching the existing
statement-visible-fast-path lifetime.

`ownerless_persist_redo_checkpoint()` treats a checkpoint update as eligible
for coalescing only when all of the following are true:

- the update is non-durable,
- the caller supplied a non-zero latest LSN and no requested visible LSN,
- the current statement is using deferred page-log append batching and started
  outside an explicit transaction,
- unsafe ownerless fault hooks are not active.

For the first eligible update, MyLite runs the existing checkpoint update path.
Only after that update succeeds does the statement mark the latest checkpoint as
preserved. Later eligible updates in the same statement increment
`checkpoint_update_deferred_latest_coalesced` and return without taking the
checkpoint update path. The final page-visible checkpoint remains durable and
unchanged.

This preserves the observable crash boundary used by existing tests: if the
process dies before page-visible publication, the checkpoint file still has at
least the first latest-only advancement from the statement. If the process
reaches page-visible publication, the durable latest/visible checkpoint is still
the recovery boundary.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, or wire-protocol behavior changes. The change is
internal to ownerless checkpoint metadata publication for an already-supported
fast path.

## Directory And Lifecycle Impact

No new files or durable formats. The new state is transient thread-local
statement state. Existing `mylite-concurrency.ckpt` records remain the durable
checkpoint source.

## Native Storage Impact

No native InnoDB, MyISAM, Aria, redo, page, or tablespace format changes. Native
mini-transaction, page-log sync, page-visible publication, and durable
checkpoint ordering are unchanged.

## Build, Size, License, And Dependencies

No dependency or license impact. Binary-size impact is limited to two
thread-local booleans, a small helper, and one performance counter.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Extend `test_ownerless_single_owner_multi_row_insert_visible_fast_path` to
  assert that the capped multi-row visible-fast insert coalesces at least one
  deferred latest-only checkpoint update while preserving append batching,
  native-support proof publication, and fast commit visibility.
- Run the focused SQL case directly.
- Run crash/recovery coverage around redo/latest and page-visible checkpoints.
- Run a reduced production performance probe and inspect the new detailed and
  summary counters.
- Run production-build guards, formatting, and whitespace checks.

## Acceptance Criteria

- The first eligible latest-only checkpoint update in a visible-fast append
  batch still uses the existing checkpoint update path.
- Later eligible latest-only checkpoint updates in the same statement are
  coalesced and counted.
- Durable page-visible checkpoint publication is not coalesced.
- Unsafe ownerless test-fault runs keep the previous latest-checkpoint behavior.
- Focused SQL and crash/recovery tests pass under production builds.

## Implementation Evidence

- `OwnerlessStatementVisibleFastPathScope` resets and restores the thread-local
  coalescing-allowed and latest-checkpoint-preserved flags with the same
  lifetime as the deferred append-batch statement.
- `ownerless_persist_redo_checkpoint()` coalesces only later latest-only,
  non-durable updates and only after one successful update has been preserved.
- `test_ownerless_single_owner_multi_row_insert_visible_fast_path` verifies the
  new counter fires for a three-row fast-path insert.
- `mylite_embedded_performance_probe` emits
  `checkpoint_update_deferred_latest_coalesced` in detailed output and compact
  autocommit/bulk summary rows.

Verification:

- `cmake --build build/php-embedded-prod --target mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe -j$(nproc)`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_single_owner_multi_row_insert_visible_fast_path`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test checkpoint-lsn-noop-elision`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test statement-checkpoint-scheduling`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test visible-checkpoint-crash`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test redo-written-crash`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test redo-latest-crash`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test redo-latest-checkpoint-crash`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=40 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4 MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  reported explicit transaction coalescing `0`, autocommit coalescing
  `2.000` per insert, and four-row bulk coalescing `8.000` per statement.
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=400 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  reported ownerless autocommit `1409.21 ops/s`, ordinary autocommit
  `999.88 ops/s`, ownerless four-row bulk `4025.19 rows/s`, ordinary four-row
  bulk `6741.57 rows/s`, and bulk row ratio `0.5971` in that local run.
- `cmake --build build/ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j$(nproc)`
- The same checkpoint and redo crash selectors passed under
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test`.
- `cmake --build build/ownerless-stress --target mylite_ownerless_cross_process_sql_test -j$(nproc)`
- `ctest --test-dir build/ownerless-stress --output-on-failure -R 'libmylite\\.ownerless-(single-owner-multi-row-insert-visible-fast-path|cross-process-checksum-stress)$'`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks And Follow-Up

- This is not cross-process group commit; every statement still publishes the
  final durable visible checkpoint.
- The broader ownerless performance gap still includes native commit,
  page-version publication, native-support history proof, and remaining
  redo/checkpoint reconciliation work.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  separate completion gaps.
