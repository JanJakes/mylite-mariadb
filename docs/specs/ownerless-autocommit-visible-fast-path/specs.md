# Ownerless Autocommit Visible Fast Path

## Problem

Ownerless autocommit writes still pay a native dirty-page flush wait at every
commit. The reduced embedded performance probe on 2026-06-08 reported ordinary
autocommit inserts around `1929.51 ops/s`, while ownerless autocommit inserts
remained around `101.73 ops/s`. Changing the ownerless visibility anchor sync
from `fsync()` to data sync reduced full sync calls but did not materially
change this bottleneck.

The immediately preceding `ownerless-autocommit-page-publish-stats` slice
proved that the simple prepared InnoDB insert workload publishes every modified
page image through the MTR page-version path: `120` autocommit page-publish
candidates, `120` published records, and zero skips or failures. The remaining
commit cost is therefore the conservative `buf_flush_wait_flushed()` bridge in
the ownerless commit path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` enters the ownerless commit bridge
  for read/write transactions with ownerless native locks or page-write locks.
  That bridge publishes transaction-deferred pages, optionally publishes dirty
  pages for DDL/index operations, then calls
  `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()` as a
  `buf_flush_wait_flushed(visible_lsn + 1)` wait followed by the
  `pages_visible` callback that syncs `mylite-concurrency.wal`, publishes the
  shared visible LSN, and persists `mylite-concurrency.ckpt`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes page-version records at
  mini-transaction commit for ownerless page writes that are not deferred to
  transaction cleanup. SQL autocommit is explicitly excluded from
  transaction-deferred page release by
  `ownerless_page_write_uses_transaction_release()`.
- The MTR path can still encounter unpublishable page classes, missing source
  pages, allocation failures, page-LSN mismatches, or page-version hook
  failures. A production fast path must preserve the flush fallback for any of
  those cases rather than relying only on the one probe workload. Instrumented
  runs also showed that stale setup-DDL state could make the first measured
  autocommit DML statement take the conservative flush path even when the
  measured MTR page-publish counters reported no skips or failures.

## Design

Add a transaction-local ownerless MTR page-publish failure flag to `trx_t`.
`mtr_t::ownerless_page_write_publish()` sets that flag whenever a page-write
candidate cannot be published to the page-version WAL for the current
transaction. Reset the flag when a pooled `trx_t` starts a new transaction so
DDL/internal setup state cannot force an unrelated later autocommit statement
onto the conservative bridge. Existing ownerless modified-page tracking remains
owned by its normal transaction lifecycle and is not cleared by the fast-path
reuse reset. The existing diagnostic counters remain opt-in; the new flag is
the production safety gate.

Add an internal `mylite_ownerless_innodb_publish_pages_visible_lsn()` helper
that invokes the existing `pages_visible` callback without first waiting for
native dirty pages to flush. The callback still performs the durable
page-version WAL sync and checkpoint update that make the visible LSN
recoverable.

In `trx_t::commit_in_memory()`, use the visible-only path only when all of the
following are true:

- ownerless hooks are active and this transaction is already in the ownerless
  commit bridge,
- the transaction had ownerless page-write ownership,
- the commit has a nonzero ownerless commit LSN,
- the transaction is not rollback/deadlock cleanup,
- the statement is not a dictionary/DDL/index operation that already requires
  buffer-pool dirty-page publication,
- no non-gate transaction-deferred ownerless modified pages remain,
- no MTR page-publish skip or failure was recorded.

All other ownerless commits keep the existing dirty-page flush bridge.

Extend the opt-in embedded performance probe diagnostics beyond raw
page-publish counts. With `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, the
probe reports commit-visibility fast/flush decisions and ownerless
page-version, page-write, logical-lock, and redo-hook timing buckets. These
diagnostics are disabled by default and keep CI/perf logs actionable when
ownerless autocommit throughput regresses.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or directory layout changes. The
observable compatibility goal is unchanged: committed ownerless autocommit
writes must be visible to peers and recoverable after process death. The fast
path changes the implementation from native-page flush proof to page-version
WAL proof only when the current transaction can prove every MTR page-write
candidate was published.

## Directory And Lifecycle Impact

No new files or durable metadata. The existing page-version WAL and checkpoint
anchors remain the recovery authority for the fast path.

## Native Storage Impact

Native InnoDB dirty pages may remain dirty after a fast-path autocommit commit.
Peer readers and no-live recovery must therefore continue to rely on the
page-version WAL for the published visible LSN until a later native checkpoint
or close-time reclamation proves the native files cover it. DDL/index
operations, explicit transaction-deferred page sets, and any unproven page
class keep the existing native flush wait.

## Build And Performance Impact

Ownerless autocommit writes that fully publish through MTR avoid a
per-commit `buf_flush_wait_flushed()` wait. The default non-ownerless path is
unchanged. The ownerless MTR hot path adds one transaction-local failure-bit
write only on skip/failure branches; opt-in timing counters add work only when
the performance probe enables them.

Measured evidence after the reset and fast-path gate shows `200/200` ownerless
autocommit commits using the visible-only path with zero flush fallbacks.
Throughput remains roughly `50-100 ops/s` for the reduced 200-insert probe while
ordinary autocommit remains around `2000 ops/s`, so this slice does not close
the ownerless autocommit performance gap. The timing buckets show the remaining
measured hook work is spread across page-write refresh/publication and
page-version WAL append, while logical table/record locks, redo hooks, and
pages-visible sync are not dominant for this workload.

## Test Plan

- Build the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` in embedded and hook presets.
- Run the reduced stats-enabled performance probe and verify ownerless
  autocommit commits select the visible-only path with no stale flush fallback.
- Run focused live visibility selectors:
  `prepared-committed-read` and `local-write-first-read`.
- Run hook crash selectors: `visible-publish-crash` and
  `visible-checkpoint-crash`.
- Run ownerless primitive CTests, `format-check`, and `git diff --check`.

## Acceptance Criteria

- Ownerless autocommit probe output reports complete MTR page publication and
  visible-only commit selection for the reduced insert workload.
- Page-publish stats still report complete publication for the reduced insert
  workload.
- The performance probe emits enough timing detail to distinguish page-version
  publication, page-write refresh, logical locks, redo hooks, and visibility
  sync in future CI logs.
- Focused live and crash visibility selectors still pass.
- The implementation documents and preserves the conservative fallback for
  page classes outside the proven MTR-published autocommit scope.

## Risks And Follow-Up

- This fast path is deliberately narrow. It does not prove broader DML/DDL,
  BLOB/compressed, generated-column, foreign-key, online DDL, or randomized
  ownerless workloads can skip the native flush bridge. Synthetic transaction
  page-write gate markers are not page-image evidence and are ignored only for
  this fast-path gate.
- Future slices can expand the fast path only after adding page-class coverage
  or runtime proof for those workloads.
- The remaining simple autocommit insert gap is still large after the flush
  skip. Follow-up performance work should focus on reducing repeated
  page-version/page-write work or proving a safe batching/coalescing policy,
  not on logical record/table lock hooks or redo-state callbacks for this
  workload.
