# Ownerless Known MTR Page Leave

## Problem Statement

The production 100-row ownerless bulk attribution probe still shows measurable
time in the no-dirty `mtr_t::commit_log()` release loop. The same sample showed
the attempted native written-LSN precheck was not useful because each ownerless
redo reservation advanced the native redo stream. The next bounded hot-path
slice is therefore the ownerless page-leave path inside the MTR memo release
loop.

Several release loops already test that a memo slot belongs to an
MTR-scoped ownerless page-write set before calling
`mtr_t::ownerless_page_write_leave()`. The helper repeats the same tracked-page
checks before forgetting the page. That keeps the generic helper safe, but it
adds redundant lookup work on the already-proven release path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`) with MyLite branch head
  `1761bd7a7aa468ab8ffdfc9425571096b4e3ce60`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::release_unlogged()`,
  `mtr_t::release()`, and the no-dirty branch of `mtr_t::commit_log()` already
  gate ownerless page leave with `ownerless_page_write_has_mtr_pages()` and
  `ownerless_page_write_has_mtr_page()`.
- `mtr_t::ownerless_page_write_leave()` then checks the same MTR page set again
  before calling `ownerless_page_write_forget_mtr_page()`.
- `ownerless_page_write_forget_mtr_page()` remains the authority that removes
  the tracked page and preserves inline-page promotion behavior introduced by
  the inline MTR page-tracking slice.

## Design

Add a narrow helper for callers that have already proven that a memo slot is an
MTR-tracked ownerless page:

```c++
void ownerless_page_write_leave_known_mtr_page(
    const mtr_memo_slot_t &slot) noexcept;
```

Both the generic and known helpers route through one private implementation:

- the generic helper keeps all existing defensive checks;
- the known helper skips the repeated `ownerless_page_write_has_mtr_pages()` and
  `ownerless_page_write_has_mtr_page()` checks;
- both helpers still use `ownerless_page_write_forget_mtr_page()` before
  deciding whether to release the shared page-write lock;
- both helpers preserve transaction-deferred release, lock-only transaction
  behavior, latch release order, and existing perf counters.

The release loops that already prove the page identity call the known helper.
No caller that lacks that proof changes behavior.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, storage file format, page-version
WAL format, checkpoint format, or recovery behavior changes. The slice only
removes redundant ownerless MTR page-tracking checks on a proven internal path.

## Directory And Lifecycle Impact

No durable files, shared-memory layout, runtime path, open/close lifecycle, or
database-directory ownership rules change.

## Native Storage Impact

Native InnoDB MTR commit, page latch release, flush-list insertion, redo write,
and ownerless page-write lock release ordering are unchanged.

## Binary Size, License, And Dependency Impact

The slice touches upstream-derived InnoDB MTR code plus docs only. It adds no
dependency and has negligible binary-size impact.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc` and
  `mtr0mtr.h`.
- Build production embedded and PHP ownerless SQL/performance targets.
- Run focused primitive/open-close and visible-fast ownerless SQL coverage.
- Run hook visible-fast/history/native-support coverage to preserve faulted
  release paths.
- Run reduced stats-enabled production probes and compare no-dirty page-leave,
  page-version, page-log, native-support, visibility, and redo counters.
- Run production-build guards, format-check, and `git diff --check`.

## Acceptance Criteria

- Release loops that already proved an MTR page use the known helper.
- Generic `ownerless_page_write_leave()` remains safe for unproven callers.
- Focused ownerless correctness coverage passes.
- Probe evidence records the no-dirty page-leave timing spread and shows stable
  publication and redo counters.

## Risks And Follow-Up

This removes repeated lookup work, not the underlying ownerless page-write lock
release itself. If timing does not move, the slice should be treated as a small
bookkeeping reduction rather than a throughput fix. Larger gains likely require
reducing ownerless redo-leave frequency or native page-version publication
volume.

## Implementation And Verification Evidence

Implementation:

- `mtr_t::ownerless_page_write_leave_known_mtr_page()` now routes proven
  MTR-tracked pages through the same low-level release helper while skipping
  the repeated membership checks kept by generic
  `ownerless_page_write_leave()`.
- `release_unlogged()`, `release()`, and the no-dirty `commit_log()` memo loop
  use the known helper only immediately after a successful
  `ownerless_page_write_has_mtr_page()` check.

Verification commands:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test mylite_embedded_open_close_test`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test mylite_embedded_open_close_test`
- `ctest --preset embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-open-close)$'
  --output-on-failure`
- direct production selectors:
  `single-owner-multi-row-insert-visible-fast-path` under embedded-prod and
  php-embedded-prod
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-primitives|ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path))$'
  --output-on-failure`

Reduced stats-enabled production probe shape:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
```

The embedded samples were noisy. The most comparable post-change embedded
sample reported:

- `2.000` page versions per statement;
- `4.500` page-log append calls per statement;
- `2.000` native-support published pages per statement;
- `1.000` fast commit-visibility publication per statement and `0.000` flush
  publications;
- `0.429 ms/statement` no-dirty loop time;
- `0.204 ms/statement` no-dirty page-leave time;
- `4.454 ms/statement` ownerless bulk `mysql_query()` time.

The PHP production sample preserved the same publication counts and reported
`0.462 ms/statement` no-dirty loop time,
`0.220 ms/statement` no-dirty page-leave time, and
`4.587 ms/statement` ownerless bulk `mysql_query()` time.

The final stats-off embedded bulk sample reported `23965.82` ownerless rows/s
and a `0.3918` ownerless/ordinary row ratio while the ordinary side was slower
than usual. Because the local spread remained high, this slice is recorded as a
redundant bookkeeping cleanup with stable focused counters, not as a proven
throughput improvement.
