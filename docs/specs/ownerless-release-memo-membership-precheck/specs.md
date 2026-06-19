# Ownerless Release Memo Membership Precheck

## Problem

The no-dirty ownerless MTR commit-log loop already stops entering
`ownerless_page_write_leave()` after the mini-transaction's page-write vector
is empty, and it checks membership before calling the helper while the vector
is still non-empty. The generic `mtr_t::release()` and `release_unlogged()`
paths still called the same helper for every page memo slot whenever ownerless
hooks were enabled.

Those calls can only release an ownerless page-write lock for pages present in
`m_ownerless_page_write_mtr_pages`. Non-member slots immediately return after
the helper proves no lock is tracked. This slice applies the existing
caller-side membership precheck to the remaining release paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::release()` releases native
  memo slots after the made-dirty commit-log path has already published page
  versions.
- `mtr_t::release_unlogged()` performs an equivalent native memo release for
  `MTR_LOG_NO_REDO` mini-transactions.
- `mtr_t::ownerless_page_write_leave()` only mutates release state after the
  page is present in `m_ownerless_page_write_mtr_pages` and
  `ownerless_page_write_forget_mtr_page()` removes it.
- The no-dirty commit-log loop already uses
  `ownerless_page_write_has_mtr_page()` as a caller-side guard for this exact
  condition.

## Scope And Non-Goals

In scope:

- Guard generic `mtr_t::release()` ownerless page-write leave calls by
  non-empty vector state, page latch type, and read-only page membership.
- Guard `release_unlogged()` ownerless page-write leave calls by non-empty
  vector state and read-only page membership.
- Preserve the existing leave helper for member pages.
- Record focused production probe evidence.

Out of scope:

- Changing page-version WAL records, checkpoint records, or replay.
- Changing native latch release order for member pages.
- Changing history-proof/native-support publication volume.
- Changing redo/checkpoint reconciliation semantics.
- Retrying the rejected broader MTR wrapper caching prototype.

## Design

Both release paths keep a loop-local `ownerless_page_leave` boolean initialized
from `m_ownerless_page_write_mtr_pages`. The release loop only calls
`ownerless_page_write_leave()` when the vector is non-empty and the current page
memo slot is a vector member. After a member page uses the helper, the boolean
is refreshed from the vector state.

Member pages still use the existing helper before native latch unlock. The
helper still owns lock-only handling, transaction-release handling, deferred
release, vector removal, and final ownerless lock release.

## Compatibility Impact

No SQL behavior, public C API, PHP API, mysqli behavior, wire protocol,
storage format, page-log format, checkpoint format, or directory lifecycle
behavior changes. This is an internal ownerless InnoDB hook overhead cleanup.

## Directory And Lifecycle Impact

No durable files, shared-memory fields, checkpoint records, page-log records,
or cleanup paths are added or changed.

## Native Storage Impact

Native MTR memo release, page latch unlock, redo handling, flush-list
publication, and ownerless page-write release ordering for member pages are
unchanged. The change skips only helper calls for pages that the same
mini-transaction did not record as ownerless page-write-lock owners.

## Build And Performance Impact

The change edits MariaDB-derived InnoDB MTR code, so the embedded MariaDB
archive must be rebuilt before production targets. The expected performance
impact is bounded to ownerless release loops with non-member page memo slots
while the MTR-owned page-write vector is still live. It does not reduce
required history-proof/native-support page publication.

## Verification Plan

- Rebuild the MariaDB embedded archive.
- Build the production ownerless SQL test and embedded performance probe.
- Run focused visible-fast, history-proof, native-support, and FK-cache SQL
  selectors.
- Run the active-reader pressure selector directly through the production SQL
  binary.
- Run the reduced stats-enabled production performance probe with page-publish
  stats.
- Run the direct temporary-table stress selector after rebuilding the
  `ownerless-stress` target.
- Run the full `ownerless-stress` preset after rebuilding the stress target.
- Run production-build guards, format check, and `git diff --check`.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  commit-race`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=100
  MYLITE_PERF_INSERT_ITERATIONS=500
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `build/ownerless-stress/packages/libmylite/mylite_ownerless_cross_process_sql_test
  temp-stress`
- `ctest --preset ownerless-stress --output-on-failure`

The reduced production probe preserved the visible-fast publication shape:
`0.500` bulk page versions per row, `2.000` page versions per four-row
statement, `0.500` published native-support pages per row, `2.000` published
native-support pages per statement, `1.000` commit-visibility fast path per
statement, and `0.000` commit-visibility flushes per statement.

Compared with the preceding four KiB delta fast-path sample, the targeted bulk
counters moved from `1485` to `1458` page-write leave calls, `2.101 ms` to
`1.989 ms` leave total, and `1.854 ms` to `1.685 ms` release time. The same
sample reported unchanged page-log append call shape at `0.766` calls per row
and `3.064` calls per statement.

Release-memo, no-dirty-loop, page-log append, and throughput timings were noisy
across short final probes and are not treated as material throughput claims.
The slice is a bounded release-path cleanup; the larger remaining targets are
history/native proof volume and redo/checkpoint reconciliation.

The first full stress attempt after the initial code edit failed
`libmylite.ownerless-cross-process-temporary-stress` with one child SIGSEGV.
That run started before the final `release_unlogged()` latch-bit tightening and
before the `ownerless-stress` target was rebuilt. After removing the failed
test directory, rebuilding the embedded archive and stress target from the
final source, the direct `temp-stress` selector passed and the full
`ownerless-stress` preset passed all 12 cases.

## Acceptance Criteria

- Generic release paths do not call `ownerless_page_write_leave()` for
  non-member page memo slots.
- Member page memo slots still use the existing helper before native latch
  release.
- Focused SQL coverage preserves visible-fast, history-proof, native-support,
  and active-reader behavior.
- Production probe publication counts remain stable.

## Risks And Follow-Up

- The improvement is deliberately small; it does not address the dominant
  native-support/history-proof publication volume.
- Future broader MTR helper caching still needs stronger proof because an
  earlier prototype regressed ownerless stress and DDL paths.
