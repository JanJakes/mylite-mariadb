# Ownerless Redo-State Batch Completion

## Problem

The visible-fast redo batch-completion slice moved native `log_write_up_to()`
out of each mini-transaction leave, but the flush still calls the fused
ownerless redo written/leave hook once per deferred range. The reduced
stats-enabled 100-row bulk probe after that slice reported `162.000`
page-write redo written/leave events per statement, zero immediate page-write
native log-write calls, and `page_write_redo_leave_hook_ms_per_statement=0.243`.
The raw database-level counters for the same sample reported `810` redo
written/leave callbacks across five 100-row statements.

The remaining hot path is therefore the repeated shared redo-state progress
latch acquisition in `mylite_ownerless_redo_state_complete_write_and_leave()`,
not native redo writing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `flush_deferred_redo_batch()` already owns a bounded thread-local array of
  deferred `(start_lsn, end_lsn, latest_lsn)` ranges. It writes native redo once
  to the maximum latest LSN, then calls the fused written/leave callback once
  per range.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_redo_written_leave_hook()` calls
  `mylite_ownerless_redo_state_complete_write_and_leave()` and persists a redo
  checkpoint when the leave advances the shared latest LSN.
- `packages/libmylite/src/ownerless_redo_state.cc`
  `mylite_ownerless_redo_state_complete_write_and_leave()` acquires the
  progress latch, calls `complete_write_locked()`, then
  `leave_active_owner_locked()`. Both helpers are already safe to call while
  the progress latch is held.
- `packages/libmylite/src/ownerless_redo_state.cc`
  `complete_write_locked()` clears the active reservation for a completed range
  and tolerates already-written ranges, which lets an error retry finish a
  range whose write completed before the matching owner leave.
- `packages/libmylite/tests/ownerless_primitives_test.c`
  already covers redo-state reserve, complete-write, complete-write-and-leave,
  completed-range coalescing, active-reservation counters, and owner cleanup.

## Design

Add a first-party redo-state batch API that completes multiple ranges and
leaves the active owner once per completed range while holding the redo
progress latch once:

- validate state, owner identity, range pointer, count, and each range;
- acquire the redo progress latch once;
- for each range, call `complete_write_locked()`;
- after each successful completed write, call `leave_active_owner_locked()`
  with `latest_lsn=0` except for the final completed range;
- report the number of fully completed written/leave ranges to the caller;
- release the progress latch and return the first error, if any.

Expose an optional MyLite-owned InnoDB batch callback beside the existing fused
written/leave hook. The existing single-range hook remains the fallback. The
InnoDB deferred-redo flush path uses the batch hook only when it is installed
and the deferred batch has more than one range. On a partial error, the hook
layer clears only the reported completed prefix and keeps the current and
remaining ranges queued for a later flush boundary.

The database callback persists at most one redo checkpoint for the batch, after
the final leave advances the shared latest LSN. It records database-level redo
written/leave timing by batch callback invocation, while the page-write
counters continue to report the logical mini-transaction events observed at the
MTR layer.

## Compatibility Impact

No SQL behavior, public C API, PHP API, WAL format, native redo format,
page-version record format, checkpoint format, or directory layout changes.
The slice changes only how production visible-fast ownerless statements
complete already-reserved ownerless redo ranges in shared memory.

Peer safety remains conservative: deferred ranges stay active until the batch
callback completes the matching written/leave work, and page-visible
publication still flushes deferred redo first.

## Native Storage Impact

Native InnoDB redo bytes and mini-transaction commit LSNs are unchanged. The
previous slice already batched native redo writes; this slice batches only the
MyLite ownerless redo-state completion work.

## Binary Size And Dependencies

No new dependencies and no shared-memory format bump. The implementation adds a
small first-party range type and a batch helper that reuses existing locked
redo-state helpers.

## Verification

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test
  mylite_embedded_ownerless_innodb_lock_hooks_test`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test
  mylite_embedded_ownerless_innodb_lock_hooks_test`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|cross-process-sql\.11))$'
  --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|native-table-wait|native-table-wait-crash|stale-drop-crash-recovery|history-proof-publish-failure-fallback))$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  sql-case test_ownerless_explicit_transaction_undo_wal_elision`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test && ctest --preset ownerless-stress
  -R '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod && git diff --check`
- `cmake --build --preset tidy-prod`

The reduced stats-enabled 100-row bulk attribution probe reported database
redo written/leave callbacks falling from `810` to `29` across the five
statements, while logical page-write written/leave events stayed at `162.000`
per statement and fallback hook calls stayed at zero. Commit-log redo-leave
time moved from `0.260 ms/statement` to `0.094 ms/statement`,
page-write redo hook time from `0.243 ms/statement` to `0.079 ms/statement`,
and ownerless `mysql_query()` remained `3.405 ms/statement`. The matching
stats-off 5000-row sample reported ownerless bulk at `33432.91 rows/s`,
ordinary bulk at `107188.95 rows/s`, and an ownerless/ordinary ratio of
`0.3119`.

## Test Plan

- Add primitive coverage proving the batch API matches separate
  complete-write-and-leave calls for contiguous ranges, advances latest only at
  the batch boundary, clears active reservations, updates refcount/remaining
  counts, and reports completed-prefix count on validation/error paths.
- Extend InnoDB hook tests so a deferred visible-fast batch uses the optional
  batch hook when installed and falls back to single-range written/leave when
  it is absent or unsafe hooks are enabled.
- Extend the visible-fast SQL selector to assert production database-level redo
  leave calls are lower than the logical page-write written/leave events, while
  unsafe hook builds keep the conservative immediate path.
- Run reduced stats-enabled and stats-off 100-row bulk probes and compare
  database-level redo written/leave calls/time, page-write logical event
  counts, query time, and visible-fast counters.
- Run focused production ownerless selectors, hook selectors, ownerless stress,
  production build guards, format/tidy checks, and `git diff --check`.

## Acceptance Criteria

- Batch completion is used only for the existing bounded deferred-redo
  visible-fast flush path.
- The redo-state progress latch is acquired once per deferred batch, not once
  per range, without changing active-owner or active-reservation semantics.
- Page-visible publication still happens only after all pending deferred redo
  ranges are completed or the flush fails without publishing visibility.
- Unsafe hook builds and missing batch callbacks keep the existing conservative
  path.
- Focused ownerless correctness and stress coverage remain green.
- Production attribution shows fewer database-level redo written/leave callback
  invocations per 100-row bulk statement while preserving logical page-write
  written/leave event volume.

## Risks

- Partial batch failure must not drop uncompleted ranges or double-decrement
  owner refcounts. The API reports a completed prefix so the hook layer can
  retain the correct suffix.
- Completing many ranges under one progress latch can extend latch hold time.
  The existing deferred batch capacity is bounded at 32 ranges, below the
  64 active-reservation slots.
- Database-level perf counters will shift from logical range counts to batch
  callback counts for this path. Page-write counters remain the logical range
  evidence.
