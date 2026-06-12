# Ownerless Empty Page-Write Leave Fast Path

## Problem

The production ownerless bulk-insert probe shows write throughput, not startup,
as the current performance bottleneck. In the CI-shaped stats-enabled sample
for 100 rows as 25 multi-row insert statements, ownerless bulk insert spent
`17.810ms` in page-write commit-log handling, including `178` no-dirty
commit-log calls and `7.055ms` in the no-dirty memo loop. The same phase spent
`3.892ms` in `ownerless_page_write_leave()` even though many memo slots do not
own an ownerless page-write lock by the time they are released.

`ownerless_page_write_leave()` currently resolves the ownerless transaction and
lock policy before it asks whether the current MTR actually acquired a
page-write lock for the page. An MTR with no ownerless page-write page vector,
or an already-empty vector, cannot have a page-write lock to release for that
slot. The hook can return earlier in that case.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/mtr0mtr.h` stores MTR-owned ownerless
  page-write locks in `m_ownerless_page_write_mtr_pages`.
- `mtr_t::ownerless_page_write_note_mtr_page()` allocates and inserts into
  that vector only after `ownerless_page_write_enter()` successfully acquires
  an MTR-scoped page-write lock.
- `mtr_t::ownerless_page_write_forget_mtr_page()` returns `false` when the
  vector is absent or the page is not present.
- `mtr_t::ownerless_page_write_leave()` only releases an external page-write
  lock after `ownerless_page_write_forget_mtr_page()` proves the page was
  MTR-acquired. Transaction-deferred pages that are not MTR-acquired remain
  transaction-scoped and are not released by this hook.
- `mtr_t::commit_log()` calls `ownerless_page_write_leave()` while releasing
  memo slots in the no-dirty commit-log branch; the production probe now
  exposes this bucket.

## Design

Add an early return in `mtr_t::ownerless_page_write_leave()` after the hook and
page-latch checks but before ownerless transaction resolution:

- if `m_ownerless_page_write_mtr_pages == nullptr`, return;
- if `m_ownerless_page_write_mtr_pages->empty()`, return.

This preserves the existing release rule: only pages recorded by
`ownerless_page_write_note_mtr_page()` can be released by the MTR leave hook.
The fast path does not change transaction-scoped page ownership, space-write
ownership, redo handling, page publication, or memo/latch release ordering.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or user-visible behavior
changes. This is an internal ownerless InnoDB hook fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory layout, page-version WAL format, checkpoint
format, or directory lifecycle changes.

## Native Storage Impact

No native InnoDB page, redo, undo, or dictionary format changes. The MTR still
releases native latches through the existing memo-release path. Only redundant
ownerless page-write release-hook work is skipped when no MTR-owned page-write
lock can exist.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc`.
- Build production `mylite_embedded_performance_probe` and focused ownerless
  SQL targets.
- Run reduced stats-on production probes before and after the change to compare
  `page_write_leave_total_ms`, `page_write_commit_log_no_dirty_loop_ms`, and
  bulk insert throughput.
- Run focused ownerless SQL selectors that exercise page-write release and
  commit visibility: multi-row insert visible fast path, history WAL proof,
  native-support page WAL elision, native reclaim, live reclaim, commit race,
  and active-reader pressure.
- Run the corresponding ownerless hook/stress selectors that cover page-write
  publication and active-reader pressure.
- Run production-build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Focused ownerless SQL and hook/stress selectors pass.
- Stats-enabled production probe still reports zero commit-visibility flushes
  and zero publish failures for the covered bulk insert shape.
- `page_write_leave_total_ms` or no-dirty loop time improves, or the spec
  records evidence showing this fast path is not the dominant residual cost.
- The diff remains limited to the ownerless MTR hook and docs.

## Verification Results

- `tools/mariadb-embedded-build build` passed and rebuilt
  `libmariadbd.a` from the edited InnoDB MTR source.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- CI-shaped stats-on production probe before the change, from the current
  branch immediately before this slice:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=100
  MYLITE_PERF_INSERT_ITERATIONS=100
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  reported ownerless bulk throughput `2050.14 rows/s`,
  `page_write_leave_total_ms=3.892`,
  `page_write_release_ms=3.772`,
  `page_write_commit_log_total_ms=17.810`,
  `page_write_commit_log_no_dirty_calls=178`,
  `page_write_commit_log_no_dirty_loop_ms=7.055`,
  `page_write_commit_log_publish_ms=9.212`, and
  `page_log_append_total_ms=6.569`.
- The same stats-on production probe after the fast path reported ownerless
  bulk throughput `3826.56 rows/s`, `page_write_leave_total_ms=0.551`,
  `page_write_release_ms=0.458`,
  `page_write_commit_log_total_ms=9.947`,
  `page_write_commit_log_no_dirty_calls=176`,
  `page_write_commit_log_no_dirty_loop_ms=5.827`,
  `page_write_commit_log_publish_ms=6.988`, and
  `page_log_append_total_ms=4.575`.
- The post-change stats-on bulk phase reported
  `commit_visibility_fast=25`, `commit_visibility_flush=0`, and
  `page_publish_failed=0`.
- Stats-off production probe after the change reported ownerless single-row
  autocommit `1475.52 ops/s` at ratio `0.4976` and ownerless bulk row-list
  insert `3063.06 rows/s` at ratio `0.3061`. The pre-change stats-off sample
  from the same local session reported ownerless single-row autocommit
  `1495.48 ops/s` at ratio `0.3424` and ownerless bulk row-list insert
  `3761.72 rows/s` at ratio `0.2678`; the row-list ratio improved, while the
  absolute samples remained noisy.
- Production focused correctness passed:
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)$'
  --output-on-failure`.
- Direct production ownerless SQL selectors passed:
  `native-reclaim`, `live-reclaim`, `commit-race`, and
  `active-reader-pressure`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- Hook crash selectors passed:
  `visible-publish-crash` and `visible-checkpoint-crash`.
- Hook CTest subset passed:
  `ctest --preset ownerless-test-hooks -R
  'libmylite\.ownerless-single-owner-(page-write-refresh-skip|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|tools\.ownerless-active-reader-pressure-trace$'
  --output-on-failure`.
- Focused stress subset passed:
  `ctest --preset ownerless-stress -R
  'libmylite\.ownerless-single-owner-(page-write-refresh-skip|history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|libmylite\.ownerless-cross-process-active-reader-pressure-stress$|tools\.ownerless-active-reader-pressure-trace$'
  --output-on-failure`.
- Production guards passed:
  `tools/check-ci-production-builds`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/ownerless-test-hooks`, and
  `tools/require-cmake-release-build build/ownerless-stress`.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu:build/php-embedded-prod/packages/libmylite:build/mariadb-embedded/lib
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.
