# Ownerless Page-Write Leave Membership Fast Path

## Problem

The current ownerless write profile still shows measurable native
page-publication overhead after page-log index-delta work. A reduced 500-row
production attribution sample at branch head `e103c2f5` reported:

- `mylite_perf_ownerless_insert_autocommit_page_write_leave_calls=2615`;
- `mylite_perf_ownerless_insert_autocommit_page_write_leave_total_ms=5.554`;
- `mylite_perf_ownerless_insert_autocommit_page_write_release_ms=4.368`;
- `mylite_perf_ownerless_insert_autocommit_page_write_commit_log_no_dirty_loop_ms=76.541`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=1506`.

The no-dirty MTR release loop must still release native memo slots, but the
ownerless leave hook was resolving transaction and deferred-release policy for
page latch slots before proving that the mini-transaction had acquired an
ownerless page-write lock for that page. Most memo slots in the hot insert
shape are not MTR-owned ownerless page-write locks by release time.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_enter()`
  records MTR-owned ownerless page-write locks in
  `m_ownerless_page_write_mtr_pages` only after the page-write lock is
  acquired.
- `mtr_t::ownerless_page_write_leave()` already returns early when the vector
  is absent or empty, but if the vector is non-empty it previously evaluated
  transaction lock-only policy, transaction-release policy, and deferred
  release before `ownerless_page_write_forget_mtr_page()` proved that the
  current page was actually in the vector.
- The deferred-release result is ignored when the page was not MTR-acquired.
  Therefore non-member page latch slots can return before ownerless
  transaction/deferred-release policy resolution without changing release
  semantics.

## Design

Add a read-only `mtr_t::ownerless_page_write_has_mtr_page()` helper that tests
the existing MTR-owned page vector without mutating it. In
`ownerless_page_write_leave()`:

1. keep the existing hook, page-latch, vector-null, and vector-empty checks;
2. compute the page id and return immediately if the page is not in the MTR
   ownerless page-write vector;
3. preserve the old lock-only, transaction-release, deferred-release,
   `ownerless_page_write_forget_mtr_page()`, and lock-release order for pages
   that are in the vector.

The change does not alter transaction-scoped page ownership, space-write
ownership, native latch release, page publication, redo handoff, or page-log
format.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, storage format, page-log format,
checkpoint, or recovery behavior changes. This is an internal ownerless InnoDB
hook fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory layout, checkpoint files, or directory
lifecycle rules change.

## Native Storage Impact

Native MTR memo-slot release and latch release ordering are unchanged. The fast
path only avoids first-party ownerless policy work for pages that the same MTR
did not record as ownerless page-write-lock owners.

## Build And Performance Impact

Stats-off runtime removes unnecessary ownerless transaction/deferred-release
policy checks for non-member page latch slots while the MTR page-write vector
is non-empty. Stats-enabled probes still count the leave call itself, so the
measurement remains visible.

A reduced 500-row production attribution sample after the change reported:

- `mylite_perf_ownerless_insert_autocommit_page_write_leave_calls=2621`;
- `mylite_perf_ownerless_insert_autocommit_page_write_leave_total_ms=4.911`;
- `mylite_perf_ownerless_insert_autocommit_page_write_release_ms=4.045`;
- `mylite_perf_ownerless_insert_autocommit_page_write_commit_log_no_dirty_loop_ms=65.279`;
- `mylite_perf_summary_ownerless_autocommit_page_versions_per_insert=3.008`;
- `mylite_perf_summary_ownerless_autocommit_page_log_append_calls_per_insert=3.012`;
- `mylite_perf_summary_ownerless_autocommit_native_support_published_pages_per_insert=2.004`;
- `mylite_perf_summary_ownerless_autocommit_commit_visibility_flush_per_insert=0.000`.

The broader timing buckets remain noisy because page-log append and native
commit work dominate the sample. Treat this as a bounded ownerless hook
overhead reduction, not completion of the write-throughput work.

## Test Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc`.
- Build production embedded primitive, performance probe, and ownerless SQL
  targets.
- Run a reduced stats-enabled production performance probe and verify page
  counts, append counts, and commit-visibility fast-path counters remain
  stable.
- Run focused ownerless SQL selectors covering multi-row visible fast path,
  history WAL proof, native-support WAL elision, native/live reclaim,
  commit-race, and active-reader pressure.
- Run hook and stress focused subsets that exercise page-write release and
  active-reader pressure.
- Run production-build guards, format-check, and `git diff --check`.

## Acceptance Criteria

- Non-member MTR page latch slots return before ownerless
  transaction/deferred-release policy resolution.
- Member MTR page latch slots preserve the old lock-only and deferred-release
  ordering.
- Page-version counts, native-support publication counts, and
  commit-visibility fast-path counters remain stable in the reduced production
  probe.
- Focused ownerless correctness and stress selectors pass.

## Risks And Follow-Up

This slice does not reduce necessary history-proof page publication, page-log
encoding, row-insert MTR commit cost, or redo/checkpoint recovery work. Those
remain the larger ownerless write-throughput and completion targets.
