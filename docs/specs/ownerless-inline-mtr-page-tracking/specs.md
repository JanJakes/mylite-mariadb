# Ownerless Inline MTR Page Tracking

## Problem Statement

The ownerless write path still spends measurable time in InnoDB
mini-transaction page-write bookkeeping after the larger page-log, checkpoint,
and release-loop fast paths. A reduced stats-enabled production bulk sample at
`e758082e` reported 10 ownerless 100-row statements with:

- `2399` page-write leave calls;
- `3684` page-write enter calls;
- `1790` no-dirty commit-log calls;
- `4.456 ms` in the no-dirty loop;
- `2.239 ms` in no-dirty page-write leave;
- `4.113 ms/statement` inside ownerless `mysql_query()`.

`mtr_t` currently tracks MTR-scoped ownerless page-write ownership only in a
lazily allocated `small_vector<uint64_t, 16>`. Many mini-transactions acquire
one ownerless page-write lock, so they still pay the vector allocation and scan
cost even though a single inline page identity would be enough.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`) with MyLite branch head
  `e758082eedd95f5c51c1b4069bcf0f3cc3566294`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_enter()`
  records MTR-owned page-write locks only after
  `mylite_ownerless_innodb_lock_acquire_page_write()` succeeds and only for
  pages that are not transaction-deferred.
- `mtr_t::ownerless_page_write_note_mtr_page()` allocates
  `m_ownerless_page_write_mtr_pages` before storing the first packed
  `(space_id, page_no)` identity.
- `mtr_t::ownerless_page_write_has_mtr_page()` and
  `mtr_t::ownerless_page_write_forget_mtr_page()` search the vector to guard
  release and erase the MTR-owned identity.
- `release()`, `release_unlogged()`, and the no-dirty `commit_log()` loop
  already keep a mutable "does this MTR have any tracked ownerless pages?"
  predicate and release native memo slots in reverse order.
- `mtr_t::ownerless_page_write_leave()` releases the external ownerless
  page-write lock only after the page-latch slot is proven to be tracked by the
  same MTR.
- `small_vector` has no `pop_back()` helper; removal uses the existing
  ordered `erase(begin, end)` path.

## Design

Keep the existing overflow vector, but add a first-page inline slot to `mtr_t`:

- `m_ownerless_page_write_inline_mtr_page`, a packed page identity;
- `m_ownerless_page_write_inline_mtr_page_set`, an explicit bit because
  `(space_id=0, page_no=0)` is a valid packed page.

The helper behavior becomes:

- `ownerless_page_write_note_mtr_page()` stores the first distinct page in the
  inline slot and returns without allocating the vector. A duplicate first page
  is ignored. A second distinct page allocates the existing vector and stores
  only overflow identities there.
- `ownerless_page_write_has_mtr_page()` first compares the inline page, then
  falls back to the overflow vector.
- `ownerless_page_write_forget_mtr_page()` clears the inline page when it is
  the requested page. If overflow pages exist, it moves the first overflow
  identity into the inline slot and erases that overflow element, preserving
  the existing ordered erase behavior for non-inline pages.
- Release loops and assertions use a new
  `ownerless_page_write_has_mtr_pages()` predicate instead of open-coding only
  the vector-null/vector-empty test.

The slice deliberately does not cache transaction-release predicates, change
page-write lock ownership lifetime, reorder release before native latch unlock,
or change `small_vector::erase()` semantics. Those shapes were implicated in
the earlier rejected `ownerless-mtr-fast-path-audit` prototype.

## Affected MariaDB Subsystems

- InnoDB mini-transaction lifecycle (`mtr_t::start()`,
  `mtr_t::release_resources()`, `mtr_t::~mtr_t()`).
- Ownerless InnoDB page-write hooks in `mtr0mtr.cc`.
- Debug/memory-instrumented MTR field initialization through `MEM_UNDEFINED`
  and `MEM_MAKE_DEFINED`.

## Compatibility Impact

No SQL behavior, public C API, PHP API, wire-protocol behavior, storage file
format, page-version WAL format, checkpoint format, or recovery rule changes.
This is an internal ownerless InnoDB bookkeeping optimization.

## Directory And Lifecycle Impact

No durable files, shared-memory segments, runtime-directory names, lock files,
or close/reopen lifecycle rules change.

## Native Storage Impact

Native page latches, MTR memo ordering, redo publication, flush-list updates,
page-version publication, history-proof publication, and ownerless page-write
lock release order are intended to remain unchanged. The vector remains
available for multi-page mini-transactions.

## Binary And Memory Impact

`mtr_t` gains one `uint64_t` and one bitfield flag. The common single-page
MTR-scoped ownerless acquisition avoids one heap allocation for the
`small_vector<uint64_t, 16>` overflow object. No new dependency is introduced.
The production performance probe also gains stats-enabled counters for inline
first-page records, inline duplicates, overflow vector allocations, overflow
page inserts, and inline-slot promotions.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc` and
  `mtr0mtr.h`.
- Build focused production ownerless SQL, primitive, open/close, and
  performance targets.
- Run focused embedded ownerless primitive/open-close and cross-process SQL
  selectors that exercise MTR page-write release, native-support/history-proof
  publication, redo visibility, and active-reader pressure.
- Run reduced stats-enabled and stats-off production performance probes to
  verify page-version, page-log, native-support, checkpoint, and commit
  visibility counts stay stable while timing remains close to branch baseline.
- Run production-build guard scripts, format-check, and `git diff --check`.

## Acceptance Criteria

- A single MTR-scoped ownerless page-write lock is tracked without allocating
  the overflow vector.
- Multi-page MTR ownership still releases every acquired ownerless page-write
  lock before native page latch release.
- Release loops stop only when both the inline slot and overflow vector are
  empty.
- Focused ownerless correctness coverage passes.
- Production probe count summaries do not show a page-version, native-support,
  checkpoint, or visibility regression.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_open_close_test
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- `ctest --preset embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-open-close)$'
  --output-on-failure`
- `ctest --preset embedded-prod -R
  'libmylite\.(ownerless-cross-process-sql\.(0|1|2))$'
  --output-on-failure`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_primitives_test
  mylite_embedded_open_close_test mylite_ownerless_cross_process_sql_test`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-open-close)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-primitives|ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path))$'
  --output-on-failure`

The final stats-enabled attribution sample used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It preserved the expected ownerless 100-row bulk write volume:

- `2.000` page-version records per statement;
- `4.500` page-log append calls per statement;
- `2.000` native-support published pages per statement;
- `90.100` native-support elided pages per statement;
- `1.000` fast commit-visible publications per statement;
- `0.000` commit-visibility flushes per statement;
- `180.500` deferred latest-checkpoint coalesces per statement.

The same sample recorded `912` inline first-page MTR records and only `10`
overflow vector allocations across 10 ownerless bulk statements, or
`91.200` inline first-page records and `1.000` overflow vector allocation per
statement. This proves the common MTR-owned page-write tracking path no longer
allocates the overflow vector.

Timing in that stats-enabled sample was:

- ownerless bulk `mysql_query()`: `3.815 ms/statement`;
- page-write commit-log: `1.199 ms/statement`;
- commit-log redo leave: `0.630 ms/statement`;
- no-dirty loop: `0.433 ms/statement`;
- no-dirty page-write leave: `0.211 ms/statement`.

The final stats-off 5000-row, 100-row-per-statement production sample reported:

- ordinary bulk: `58432.22 rows/s`;
- ownerless bulk: `22961.08 rows/s`;
- ownerless/ordinary bulk ratio: `0.3930`.

Local stats-off samples remained noisy because ordinary bulk varied widely, so
this slice claims the allocation-path reduction and stable correctness counts,
not a broad throughput guarantee.

The same stats-enabled probe shape under `php-embedded-prod` recorded the same
allocation-path counters (`91.200` inline first-page records and `1.000`
overflow vector allocation per statement), stable page-version/page-log/native
support/visibility counts, and noisier ownerless bulk `mysql_query()` at
`4.968 ms/statement`.

## Risks And Follow-Up

This is a bounded micro-optimization. It does not complete ownerless
concurrency, eliminate native-support/history-proof publication, broaden SQL
coverage, or replace the remaining external MariaDB/RQG stress gap. If timing
does not improve or stress coverage flakes, the code must be reverted rather
than weakening page-write release semantics.
