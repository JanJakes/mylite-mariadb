# CI Production Row Insert Attribution

## Problem

The ownerless branch now splits CI timing steps and uses production build
presets for first-party, embedded, and WordPress timing paths. The workflow
audit rejects developer presets and stale WordPress timing structure, but it
mostly proves marker presence rather than proving that each timing-producing
step carries the build-type guards inside its own step body.

The latest production attribution also shows the ownerless autocommit gap is no
longer explained by a single page-log append subphase. Ownerless commit and row
insert remain meaningful deltas versus ordinary InnoDB, but the compact summary
still reports row insertion mostly as one `row_insert_for_mysql()` bucket plus
clustered optimistic B-tree time. That is not enough evidence to choose a safe
engine optimization.

## Source Findings

- `.github/workflows/ci.yml` uses `prod` for the normal CMake matrix,
  `php-embedded-prod` for embedded tests and probes, and a Release WordPress
  PHP-extension build in `build/wordpress-php-embedded-prod`.
- `tools/check-ci-production-builds` rejects developer CMake presets, developer
  build directories, all-in-one WordPress timing phases, stale broad PHPUnit
  filters, and parallel embedded CTest. Before this slice it also required the
  guard literals globally.
- `tools/require-cmake-release-build` reads `CMAKE_BUILD_TYPE` from first-party
  CMake caches and requires `Release`.
- `tools/require-cmake-build-type` reads `CMAKE_BUILD_TYPE` from generated
  MariaDB embedded caches and requires the expected optimized type, `MinSizeRel`
  for CI MariaDB archives.
- `tools/wordpress-phpunit-mysqli-mylite` forwards
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE` into CMake, and when
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1` it rejects non-Release MyLite
  builds and non-`MinSizeRel` MariaDB embedded archives before dependency,
  database-prep, perf-probe, and PHPUnit phases proceed.
- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0mysql.cc` routes SQL row writes through
  `row_insert_for_mysql()`, and the branch already instruments transaction
  start, prebuilt handling, row conversion, row-step execution, error handling,
  and post-processing time.
- `mariadb/storage/innobase/row/row0ins.cc` routes the row graph through
  `row_ins_step()`, `row_ins()`, `row_ins_index_entry_step()`,
  `row_ins_index_entry()`, clustered and secondary index entry helpers, and
  optimistic or pessimistic B-tree insertion. Existing deep counters already
  cover those paths.
- `packages/libmylite/tests/embedded_performance_probe.c` already emits raw
  ordinary and ownerless deep-counter values when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and the CI ownerless
  attribution probe runs that mode separately from the stats-off throughput
  probe under `php-embedded-prod`.

## Design

Strengthen the CI audit structurally:

- keep the existing global forbidden-pattern and required-marker checks,
- add step-scoped checks that the regular CMake `Test` step contains the
  Release cache guard,
- add step-scoped checks that embedded non-ownerless tests, ownerless SQL,
  performance, and attribution probes all contain both the MariaDB `MinSizeRel`
  guard and the MyLite `Release` guard,
- add step-scoped checks that WordPress dependency, database-prep, perf-probe,
  and all PHPUnit test-only steps contain both the WordPress MariaDB
  `MinSizeRel` guard and the WordPress MyLite `Release` guard, and
- add step-scoped checks for clang-format and clang-tidy production guards.

Extend only the stats-enabled embedded attribution output. Reuse existing deep
InnoDB counters and emit compact ordinary, ownerless, and
ownerless-minus-ordinary per-insert summaries for:

- `row_insert_start_trx`,
- `row_insert_prebuilt`,
- `row_insert_convert`,
- `row_insert_step`,
- `row_insert_error`,
- `row_insert_post`,
- `row_ins_step`,
- `row_ins`,
- `row_ins_index_entry_step`,
- `row_ins_index_entry`,
- `row_ins_clust_entry`,
- `row_ins_sec_entry`,
- `row_ins_clust_low`,
- `row_ins_sec_low`, and
- clustered pessimistic B-tree insertion.

Keep the existing total row-insert and clustered optimistic B-tree keys
unchanged. The ownerless total row-insert and clustered optimistic B-tree
summary keys are already emitted by the ownerless phase summary, so the
comparison helper continues to avoid duplicating those keys.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, native storage format, metadata, locking, or
directory-lifecycle behavior changes. The changes affect CI workflow auditing
and opt-in performance diagnostics only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The performance probe continues to create
and remove temporary database directories. The WordPress CI job continues to
require its timing database outside the repository worktree.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage behavior changes. The row-insert counters observe existing
MariaDB/InnoDB execution paths without changing B-tree, undo, redo, checkpoint,
or page-publication behavior.

## Build And Performance Impact

CI timing claims become harder to accidentally invalidate because the audit now
requires production guards in the timing step bodies, not just somewhere in the
workflow file.

The new attribution keys are printed only when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. They add counter reads already
performed by the probe and extra `printf()` calls after measured loops. The
default stats-off throughput probe remains unchanged.

## Test And Verification Plan

- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`.
- Build `mylite_embedded_performance_probe` under `php-embedded-prod`.
- Run a reduced stats-enabled production attribution probe and confirm new
  row-insert subphase summary keys are present.
- Run a reduced stats-off production probe and confirm attribution-only
  subphase keys are absent while throughput output still runs.
- Run focused ownerless primitive/history tests that do not require unsafe hook
  builds.
- Run production formatting and whitespace checks.

## Acceptance Criteria

- Any CI workflow edit that removes production guards from a timing-bearing
  step fails `tools/check-ci-production-builds`.
- The stats-enabled production attribution probe reports row-insert subphase
  ordinary, ownerless, and ownerless-minus-ordinary summary keys.
- The stats-off production throughput probe does not emit the new
  attribution-only subphase summaries.
- Documentation continues to state that full ownerless concurrency is not
  complete until the remaining correctness and recovery gaps are closed.

## Risks And Unresolved Questions

- The new subphase summaries explain where time is spent but do not themselves
  optimize it.
- Row-insert overhead may still be caused by ownerless page publication or MTR
  commit side effects that appear under B-tree insertion rather than in an
  obviously MyLite-owned function.
- Broader native redo/checkpoint reconciliation, durable DDL file-lifecycle
  recovery, active-reader pressure policy, and longer external randomized
  MariaDB/RQG stress remain separate completion gaps.
