# Ownerless Consistent Snapshot Live Pin

## Problem

`START TRANSACTION WITH CONSISTENT SNAPSHOT` opens its ownerless page-version
pin before MariaDB executes the SQL statement. A newly opened handle may have
only observed the durable page-visible LSN from `.ckpt`, while shared redo
state has a newer committed ownerless latest LSN retained in
`mylite-concurrency.wal` because older snapshot readers are still active. In
that shape, the pre-execution pin can select the old visible boundary and the
new repeatable-read transaction sees stale rows even though it starts after
the writer committed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`:
  `ensure_ownerless_consistent_snapshot_start_pin()` previously used only
  `ownerless_handle_observed_read_lsn()`, then fell back to native checkpoint
  baseline seeding when that handle had not observed an ownerless boundary.
- `packages/libmylite/src/database.cc`:
  `refresh_ownerless_external_pages_before_statement()` already permits
  eligible page-version reads to use the live ownerless read LSN
  `max(latest_lsn, visible_lsn)` when native write state is idle and older
  external pins retain the durable visible boundary.
- `packages/libmylite/src/database.cc`:
  `update_ownerless_transaction_state_after_successful_sql()` records the
  active transaction snapshot boundary after `START TRANSACTION` succeeds, so
  it must preserve any LSN chosen by the pre-execution pin.

## Design

When a consistent-snapshot transaction starts, read the shared ownerless redo
state while holding the runtime mutex. Always consider the shared visible LSN.
If the shared ownerless transaction registry has no active write transactions
and the redo state has no active redo reservations, allow the pre-execution
pin to use the live ownerless read LSN. Keep the existing monotonic handle
rule, native checkpoint baseline fallback, and transaction pin registry.

After MariaDB accepts `START TRANSACTION WITH CONSISTENT SNAPSHOT`, preserve
the pre-registered transaction pin LSN as
`ownerless_transaction_snapshot_visible_lsn` instead of replacing it with the
fresh handle's older observed value.

## Compatibility Impact

This aligns ownerless repeatable-read semantics with MariaDB expectations: a
new consistent snapshot sees commits that completed before the transaction
started, even while older ownerless snapshots retain WAL for their own view.
No public API, SQL syntax, durable file layout, or native storage format
changes.

## Directory And Lifecycle Impact

The slice uses existing ownerless shared-memory redo state and page-version
pin registry segments. No new files or directory entries are added.

## Native Storage Impact

Native InnoDB behavior is unchanged. The fix only selects the correct MyLite
page-version overlay boundary before MariaDB creates the transaction read
view.

## Test And Verification Plan

- Run `active-pin-reclaim-boundary`, which starts older repeatable-read
  snapshots, commits newer ownerless data, then starts a later consistent
  snapshot that must see the newer committed rows while still retaining WAL.
- Run adjacent native reclaim crash/race selectors.
- Run the hook crash-tail aggregate.
- Run focused ownerless hook CTests and production build checks.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- A fresh `START TRANSACTION WITH CONSISTENT SNAPSHOT` pins the live ownerless
  read LSN when no ownerless writer/redo reservation is active.
- The transaction state after successful `START TRANSACTION` keeps the
  pre-execution pin LSN.
- Older snapshot pins still retain WAL until release.
- The active-pin reclaim-boundary selector and native reclaim crash/race
  selectors pass.

## Verification Results

Completed on 2026-06-13:

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- Direct selectors passed: `active-pin-reclaim-boundary`,
  `consistent-snapshot-pin-race`, `native-reclaim`,
  `native-reclaim-crash`, and `native-reclaim-race`.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-primitives)$'
  --output-on-failure` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- The live LSN remains disallowed while ownerless write transactions or redo
  reservations are active; broad write/read interleavings remain covered by
  the existing active-reader pressure and transaction stress suites.
- The full hook `crash-tail` aggregate still has a later
  `record-lock-grant-crash` lock-ordering failure unrelated to this snapshot
  pinning fix.
