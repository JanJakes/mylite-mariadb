# Ownerless Visible-Fast Redo Batch Completion

## Problem

Ownerless 100-row bulk insert profiling still shows a large write-path gap
after page-version WAL append, history-proof, and page-write classification
optimizations. The current reduced attribution probe reports about `162`
ownerless redo-leave events per 100-row statement, with
`page_write_commit_log_redo_leave_ms_per_statement=0.634` split almost evenly
between native `log_write_up_to()` and the ownerless redo-state hook.

The existing fused written/leave hook removed one shared redo-state latch
acquisition per mini-transaction, but each mini-transaction still forces native
redo to be written and completes the ownerless redo reservation immediately.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_redo_leave()` calls `log_write_up_to(m_commit_lsn, false)`
  before the ownerless redo written/leave hook. The mini-transaction commit LSN
  is the end of the appended redo range from `finish_writer()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `finish_writer()` reserves an ownerless redo range before appending the native
  redo bytes and stores the reserved `(start_lsn, end_lsn)` on the `mtr_t`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  owns the statement-local visible-fast flags and the nested ownerless redo
  depth. It already calls `log_write_up_to()` for ownerless external-LSN
  advancement, so a MyLite-owned hook helper can batch native redo writes
  without exposing MariaDB's internal group-commit lock.
- `packages/libmylite/src/ownerless_redo_state.cc`
  records active redo reservations and only advances `written_lsn` when the
  completed range is contiguous. Leaving an owner active while a range is
  deferred is safe for peers because the owner remains visible as active and the
  range remains uncompleted.
- `packages/libmylite/src/database.cc`
  `OwnerlessStatementVisibleFastPathScope` already brackets the SQL statements
  that are allowed to use append batching and deferred page publication.
  `mylite_ownerless_innodb_publish_pages_visible_lsn()` is called during commit
  before a visible LSN is published to the shared redo state.

## Design

Add a bounded thread-local ownerless redo completion batch in the InnoDB hook
layer. The batch is active only when all of the following are true:

- ownerless hooks are enabled;
- unsafe ownerless test faults are disabled;
- the thread is inside a statement that already enables deferred page
  publication;
- the current ownerless redo depth is the top-level depth;
- the fused written/leave hook and callback context are installed.

For eligible mini-transactions, `mtr_t::ownerless_redo_leave()` asks the hook
layer to defer the `(start_lsn, end_lsn, latest_lsn)` completion instead of
calling `log_write_up_to()` and completing the range immediately. The hook layer
decrements the thread-local redo depth immediately so the next mini-transaction
still enters the shared redo state normally. It keeps the shared active owner
entry and reservation slot live until the deferred batch is flushed.

The batch has a fixed capacity below the existing 64 active-reservation slots.
When the batch reaches capacity, before page-visible publication, on statement
scope teardown, or before hooks are reset, the hook layer:

1. writes native redo once up to the maximum deferred latest LSN;
2. completes each deferred range through the existing fused written/leave hook;
3. passes `latest_lsn=0` for all but the final range, so the shared latest LSN
   and checkpoint are advanced once per batch instead of once per mini-
   transaction.

The existing conservative path remains active for unsafe hook builds, non-
visible-fast statements, nested redo, missing callbacks, full batches that fail
to flush, DDL/file lifecycle paths, and explicit fallback calls.

If a flush encounters an unexpected non-OK result after completing part of a
batch, the hook layer keeps the current and remaining ranges queued so a later
flush boundary does not forget still-active ownerless redo reservations.

## Compatibility Impact

No SQL behavior, public C API, PHP API, WAL format, native redo format,
page-version record format, checkpoint format, or directory layout changes.
The change delays ownerless redo completion inside an already-proven statement
boundary while preserving the rule that page-visible LSN publication is not
attempted until native redo has been written and ownerless redo reservations
have been completed.

Peers may observe the writer as active for slightly longer inside the statement,
which is conservative. Crash recovery remains conservative because deferred
ranges are either completed before visibility or remain active/uncompleted until
owner cleanup/reopen recovery handles them.

## Native Storage Impact

Native InnoDB redo bytes are appended at the same mini-transaction boundaries.
This slice only batches the native write-to-file request and ownerless
redo-state completion for bounded visible-fast statements.

## Binary Size And Dependencies

No new dependencies. The implementation adds a small fixed thread-local array
in the existing InnoDB hook translation unit.

## Verification

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision))$'
  --output-on-failure`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|native-table-wait|native-table-wait-crash|stale-drop-crash-recovery)$'
  --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-history-proof-publish-failure-fallback$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`

The reduced stats-enabled 100-row bulk attribution probe reported
`page_write_commit_log_ms_per_statement=0.725`,
`page_write_commit_log_redo_leave_ms_per_statement=0.260`,
`page_write_redo_leave_log_write_calls_per_statement=0.000`,
`page_write_redo_leave_hook_ms_per_statement=0.243`,
`page_write_redo_leave_written_hook_calls_per_statement=162.000`, zero fallback
hook calls, and `mysql_query_ms_per_statement=3.406`. The matching stats-off
5000-row sample reported ownerless bulk at `27297.54 rows/s`, ordinary bulk at
`96781.94 rows/s`, and an ownerless/ordinary ratio of `0.2821`.

## Test Plan

- Add focused SQL assertions to the existing single-owner visible-fast multi-row
  insert selector showing native redo write calls are lower than ownerless
  written/leave events and that visible-fast commit still publishes rows without
  conservative flush.
- Preserve unsafe hook coverage by keeping test-fault builds on the existing
  immediate written/leave path.
- Run production focused ownerless primitives, visible-fast, history-proof, and
  native-support selectors.
- Run the hook-build focused selectors and ownerless stress preset.
- Run reduced production stats-enabled and stats-off 100-row bulk probes and
  compare redo-leave/log-write counts, query time, and visibility counters.
- Run production build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Deferred redo completion is used only inside the bounded visible-fast
  statement scope.
- Page-visible publication flushes any pending ownerless redo completions before
  publishing shared visibility.
- Statement teardown and hook reset cannot leave pending thread-local redo
  batches behind.
- Focused ownerless correctness and stress coverage remain green.
- The production attribution probe shows fewer native redo write calls per
  100-row bulk statement without changing page-version/native-support/commit
  visibility volume.

## Risks

- A missed flush boundary could leave active ownerless redo reservations until
  runtime cleanup. The implementation must flush before visibility publication,
  statement teardown, and hook reset.
- Batching too many ranges can exhaust the shared active-reservation slots. The
  fixed batch limit must stay below the existing 64-slot reservation capacity.
- Unsafe fault coverage relies on the old per-window written/leave ordering, so
  unsafe hook builds must bypass the deferred path.
