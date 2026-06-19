# Ownerless Page-Write Release Tracking Elision

## Problem

Ownerless SQL statements track page-write transaction ids in the `mylite_db`
handle so the SQL wrapper can release page-write locks after successful
autocommit statements, transaction-ending statements, or statements that did not
retain a local-write transaction. That wrapper cleanup is a necessary fallback
when a statement exits without the normal InnoDB commit cleanup path.

On the normal successful InnoDB commit path, however, MariaDB already releases
the transaction's page-write locks before returning to the SQL wrapper. The
wrapper then sees the still-tracked transaction id and calls the page-write lock
registry release path again. That second call usually releases zero locks but
still enters the shared registry latch and scans the page-write lock slots.
Production probe attribution showed the ownerless write path is still too slow,
so this duplicated per-statement cleanup needs to be removed before broader WAL
publication changes.

## Source Findings

- MariaDB base: MariaDB 11.8 LTS import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` keeps ownerless page-write lock
  release inside `trx_t::commit()` when ownerless hooks are enabled. The commit
  path publishes ownerless visibility and then calls `release_locks()` for
  ordinary transactions, or
  `mylite_ownerless_innodb_lock_release_transaction_page_writes()` for
  dictionary operations.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements `mylite_ownerless_innodb_lock_release_transaction_page_writes()`,
  which releases both the ownerless page-write transaction id and the ordinary
  transaction id when they differ. The helper reaches the first-party
  `ownerless_innodb_lock_release_page_writes_hook()`.
- `packages/libmylite/src/database.cc` records page-write transaction ids in
  `ownerless_innodb_lock_acquire_page_write_hook()` while
  `OwnerlessStatementPageWriteTrackingScope` is active. Prepared and direct SQL
  wrappers call `release_ownerless_page_write_trx_ids()` after successful SQL
  when the statement boundary should not retain page-write locks.
- `packages/libmylite/src/ownerless_innodb_lock_registry.cc`
  `mylite_ownerless_innodb_lock_registry_release_transaction_records()` returns
  success after scanning transaction records and reports the released count
  separately. A redundant release for a transaction that InnoDB already cleaned
  up still pays the latch and scan cost.

## Scope

In scope:

- Clear a tracked SQL-layer page-write transaction id when the native bulk
  release hook succeeds or finds the records already absent during the active
  statement.
- Keep `release_ownerless_page_write_trx_ids()` as the fallback for paths that
  never reach the native bulk release hook.
- Add database performance counters for tracked-release calls, elapsed time,
  empty fast-path calls, fallback transaction ids released by the wrapper, and
  transaction ids cleared by the native release hook.
- Emit the new counters through the production embedded performance probe and
  compact ownerless autocommit summary rows.
- Keep the ownerless SQL weighted-shard verification runnable under CI's
  `--parallel 2` selector by isolating the shard that contains the FK
  cross-schema child-rename case, which is fast alone but can hit the per-case
  timeout under sustained parallel ownerless SQL pressure.

Out of scope:

- Changing InnoDB lock release ordering, registry layout, SQL transaction
  semantics, page publication, WAL format, redo/checkpoint behavior, or
  directory layout.
- Pairing native-support history-proof WAL publication. That remains a separate
  performance slice.

## Design

`ownerless_innodb_lock_release_page_writes_hook()` now stores the registry
result before mapping it to a hook result. If the registry release returns
`MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK` or
`MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND` while
`ownerless_current_statement_db` is set, it removes the released transaction id
from that handle's `ownerless_page_write_trx_ids` vector.

The SQL wrapper still calls `release_ownerless_page_write_trx_ids()` at the
same statement boundary. On the normal commit path, the vector is already empty
and the wrapper returns through the empty fast path. On failed, rolled-back, or
unusual paths that do not pass through the native bulk hook, the vector remains
populated and the wrapper retains the existing registry release behavior.

The new database perf counters are appended after the prepared-reset counters so
existing earlier stat indexes remain stable:

- `page_write_tracked_release_calls`
- `page_write_tracked_release_ms`
- `page_write_tracked_release_empty`
- `page_write_tracked_release_trx_ids`
- `page_write_tracked_release_native_cleared`

The ownerless cross-process SQL test keeps a sparse copy of selected database
perf-stat indexes. That sparse enum is updated with the appended counters so
later checkpoint and page-publish assertions continue reading the intended
statistics. The same test's disabled-stats coverage asserts that the new
tracked-release counters remain zero when database perf stats are disabled.

The weighted SQL shard that currently contains
`test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary`
is marked `RUN_SERIAL` in CTest. CI still invokes the ownerless SQL selector
with `--parallel 2`; CTest only prevents that known parallel-pressure-sensitive
shard from overlapping other ownerless SQL shards.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, WAL-format, native-page, or
directory-layout change. The release operation is idempotent at the registry
level; this slice removes redundant first-party bookkeeping work after native
commit cleanup succeeds.

## Native Storage Impact

Native InnoDB remains the authority for commit-time page-write lock release.
The first-party SQL wrapper keeps its fallback release for statement paths that
do not reach native commit cleanup. This preserves cross-process page-write lock
cleanup while reducing registry latch/scanning work on the normal autocommit
and transaction-ending path.

## Test Plan

- Rebuild `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` under the production embedded
  preset.
- Run a reduced production stats-enabled performance probe and verify the new
  raw and summary fields are emitted, with native-cleared ids replacing wrapper
  fallback transaction ids on the ownerless autocommit path.
- Run ownerless primitive coverage for registry invariants.
- Run the embedded ownerless cross-process SQL subset to cover autocommit,
  transaction, DDL, active-reader, and recovery-shaped ownerless paths.
- Confirm the FK cross-schema child-rename case and weighted shard pass
  isolated after any full-selector timeout.
- Confirm the full ownerless SQL selector passes with CI's production preset
  and `--parallel 2` after shard isolation.
- Run formatting and diff checks.

## Acceptance Criteria

- Normal successful ownerless autocommit statements clear tracked page-write
  transaction ids in the native bulk release hook.
- The SQL wrapper still calls the fallback cleanup function but reports the
  empty fast path instead of redundant transaction-id release work for normal
  autocommit statements.
- Focused ownerless SQL and primitive coverage remains green.
- The production performance probe exposes enough counters to detect a future
  regression back to duplicate wrapper registry scans.

## Verification

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed after rebuilding the changed C/C++ targets.
- A reduced production stats-enabled probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=10`,
  `MYLITE_PERF_INSERT_ITERATIONS=120`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` emitted the new raw and summary
  fields. The ownerless autocommit phase reported
  `page_write_tracked_release_calls=120`,
  `page_write_tracked_release_empty=120`,
  `page_write_tracked_release_trx_ids=0`,
  `page_write_tracked_release_native_cleared=120`, and
  `page_write_tracked_release_ms=0.007`, with the compact summary showing
  `1.000` native-cleared and `0.000` fallback transaction ids per insert.
- The same reduced probe sample reported ordinary autocommit inserts at
  `3164.90 ops/s`, ownerless autocommit inserts at `1662.31 ops/s`, ordinary
  bulk insert rows at `12131.44 ops/s`, and ownerless bulk insert rows at
  `5240.83 ops/s`. That confirms this slice removed duplicated cleanup work
  but the remaining ownerless gap is still dominated by page publication,
  proof-WAL, and native commit work.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- The first ownerless SQL selector run exposed a stale sparse perf-stat enum in
  `ownerless_cross_process_sql_test.c`; shard 12 failed while reading the wrong
  checkpoint coalescing counter. Updating the sparse enum and disabled-stats
  assertions fixed the issue, and `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.12$' --output-on-failure` passed.
- Two full `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` attempts then reproduced a timeout in
  `test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary`
  inside weighted shard 11. The exact case passed isolated, and shard 11 passed
  isolated, so the failure was sustained parallel-suite pressure rather than a
  deterministic SQL regression.
- After marking weighted shard 11 `RUN_SERIAL`, the same CI selector
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` passed 16/16 in `176.42` seconds.

## Risks And Follow-Up

The main risk is accidentally clearing a tracked transaction id before the page
write registry has actually been released. The implementation only clears after
the registry reports success or idempotent absence. Errors and timeouts keep the
tracked id so wrapper cleanup can still report failure.

The next higher-impact write-path slice should evaluate paired publication of
the two native-support history-proof WAL records while preserving the current
two-record durable evidence model.
