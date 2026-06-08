# Production Performance Summary

## Problem

The ownerless concurrency branch now has several performance probes, but their
raw logs are noisy: the embedded probe prints detailed open, SQL, page-publish,
commit, append, scan, handler, and deep InnoDB counters, while the WordPress
mysqli probe prints process-start, connect, and SQL loop timings. CI already
splits the WordPress PHPUnit work into separate build, dependency, database,
performance-probe, and test-only steps, and those steps run production builds,
but the logs still require manual extraction before branch/main comparisons are
useful.

The immediate performance question is whether the branch is slow because of
build/setup visibility, per-process startup/connect cost, ordinary embedded
engine execution, or ownerless-native page publication. This slice adds compact
summary keys to the existing production probes so CI and local audits can
compare the same high-signal numbers without weakening the detailed counters.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` uses `cmake --preset prod`, `cmake --build
  --preset prod`, and `ctest --preset prod` for the normal build matrix.
- `.github/workflows/ci.yml` uses `php-embedded-prod` for embedded CI
  configure, build, CTest, ownerless SQL, and the embedded performance probe.
- `.github/workflows/ci.yml` sets
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod` and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` for the WordPress PHPUnit job,
  and the harness forwards that build type into its CMake configure step.
- `.github/workflows/ci.yml` already separates WordPress source fetch, MyLite
  PHP extension build, WordPress/PHPUnit dependency install, database
  preparation, mysqli performance probe, `Tests_DB`, isolated tests, and the
  remaining non-isolated suite into separate visible CI steps.
- `tools/wordpress-phpunit-mysqli-mylite` defaults
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE` to `Release`, so local runs that do not
  override it still use optimized PHP extension builds.
- `packages/libmylite/tests/embedded_performance_probe.c` already measures
  ordinary and ownerless warm open/close, active-runtime reconnect, direct and
  prepared `SELECT 1`, transactional inserts, and autocommit inserts. When
  stats are enabled it also emits detailed page-publish, page-log append,
  page-log scan, commit-visibility, handler, and deep InnoDB counters.
- `docs/specs/ownerless-page-publish-volume-profile/specs.md` records that a
  reduced stats-enabled ownerless autocommit sample is not dominated by
  repeated page identities: 3203 page-version publishes contained 3198 unique
  `(space_id,page_no,visible_lsn)` fingerprints and only 5 duplicates.
- A follow-up local production prototype that skipped native-support page
  publication under the existing single-owner/no-pin proof cut the 400-row
  stats-enabled sample from 3203 page publishes to 400 and page-log append work
  from about `82 ms` to `16 ms`, but did not improve throughput. The same
  prototype inflated write-history timing in one stats-enabled run and a
  2000-row stats-off run regressed ownerless autocommit throughput to about
  `168 ops/s` versus the prior baseline around `295 ops/s`. That path is not a
  valid optimization without new redo/checkpoint evidence.

## Design

Keep every existing detailed metric key unchanged. Add compact summary keys at
the end of the probes:

- Embedded C API probe:
  - ordinary and ownerless warm open/close average milliseconds,
  - ordinary and ownerless active-runtime reconnect average milliseconds,
  - ownerless overheads for both open/close measurements,
  - ordinary and ownerless direct and prepared `SELECT 1` throughput,
  - ownerless/ordinary read throughput ratios,
  - ordinary and ownerless transactional and autocommit insert throughput,
  - ownerless/ordinary write throughput ratios,
  - when detailed ownerless stats are enabled, ownerless autocommit per-insert
    summaries for page-version volume, native-support page ratio, page-publish
    hook/append/index time, page-log append time, page-write refresh/publish
    time, commit-MTR publish time, InnoDB write-history time, ownerless
    visibility time, row-insert time, and clustered optimistic B-tree time.
- WordPress mysqli probe:
  - stock PHP process startup,
  - PHP process startup with MyLite extensions loaded,
  - derived extension-load process overhead,
  - PHP process plus mysqli connect/close,
  - derived process/connect delta,
  - in-process mysqli connect/close,
  - active-runtime reconnect,
  - steady `SELECT 1`, transactional insert, point select, prepared autocommit
    insert, and direct autocommit insert throughput,
  - direct/prepared autocommit insert throughput ratio.

Use `mylite_perf_summary_*` and `wordpress_perf_summary_*` prefixes so CI log
scraping can distinguish high-signal summaries from detailed phase counters.
The summary values are derived from the same measured intervals that the probes
already print.

Do not change the CI build matrix in this slice because the current workflow is
already production-build based for timing-sensitive jobs. The slice documents
that audit and strengthens the probes that run under those production builds.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, storage-engine, metadata, or directory-lifecycle
behavior changes. The slice changes diagnostics emitted by test/performance
harnesses only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The embedded performance probe continues
to create and remove its temporary MyLite directory. The WordPress probe
continues to reuse the prepared WordPress MyLite database directory and drops
its probe tables before closing the connection.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage format changes. The slice records that native-support
page-publication suppression was rejected as a performance optimization until
native redo/checkpoint reconciliation can prove that the reduced page images are
both correct and faster in production throughput samples.

## Wire-Protocol Or Integration-Package Impact

The WordPress mysqli harness prints additional summary lines. The mysqli PHP
extension behavior is unchanged.

## Build And Performance Impact

The probes do a few arithmetic operations and `printf()` calls after existing
measurements. The cost is outside the measured SQL loops and is negligible
compared with process startup, embedded open/close, and database work.

CI timing remains production-build based:

- first-party matrix jobs use the `prod` preset,
- embedded ownerless, embedded performance, and ownerless attribution jobs use
  `php-embedded-prod`,
- WordPress PHP extensions use `CMAKE_BUILD_TYPE=Release` in
  `build/wordpress-php-embedded-prod`,
- clang-format and clang-tidy configure through the `prod` preset and run the
  production check targets.

CI now also runs `tools/require-cmake-release-build` against the generated
CMake cache for each CMake-backed job so timing-sensitive steps fail early if a
workflow edit or reused build directory stops producing `Release` artifacts.

The embedded job keeps the default stats-off performance probe as the
throughput signal and runs a second reduced
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` attribution probe so CI logs also
include the ownerless autocommit phase summaries without conflating them with
the stats-off throughput sample.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build the production embedded performance probe with
  `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`.
- Run a reduced production embedded performance probe and confirm
  `mylite_perf_summary_*` lines are printed.
- Run a reduced production WordPress mysqli `perf-probe` after database
  preparation and confirm `wordpress_perf_summary_*` lines are printed.
- Run focused ownerless primitive CTest coverage under `php-embedded-prod`.
- Run `cmake --build --preset format`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-08 used the production
`build/php-embedded-prod` and `build/wordpress-php-embedded-prod` artifacts.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- A reduced production embedded performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=20`,
  and `MYLITE_PERF_INSERT_ITERATIONS=20` passed and printed
  `mylite_perf_summary_*` lines, including warm open/close, active-runtime
  reconnect, direct/prepared read throughput, transactional insert throughput,
  and autocommit insert throughput summaries.
- A reduced production WordPress mysqli `perf-probe` with the pinned CI
  WordPress ref `6ddfc9d9b532c6e95c1266165149815895e2eb56`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`, one process/connect iteration,
  five SQL iterations, and two write iterations passed and printed
  `wordpress_perf_summary_*` lines, including process startup, process/connect
  delta, in-process connect, active-runtime reconnect, and SQL loop throughput
  summaries.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-primitives$' --output-on-failure` passed.
- `cmake --build --preset format` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed again after formatting.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

A follow-up CI guard slice also verified that
`tools/require-cmake-release-build` accepts local `build/prod`,
`build/php-embedded-prod`, and `build/wordpress-php-embedded-prod` Release
caches, rejects a temporary Debug cache, and keeps the production timing
documentation aligned with the workflow.

## Acceptance Criteria

- CI and local production probes emit compact summary keys for startup,
  reconnect, read throughput, and write throughput.
- Existing detailed metric keys remain unchanged.
- Stats-enabled ownerless autocommit probes emit per-insert phase summaries
  derived from existing detailed counters.
- CI timing-sensitive jobs remain production-build based and test-only
  WordPress PHPUnit steps remain separated from build/setup phases.
- CI separates the embedded stats-off throughput probe from the reduced
  stats-enabled ownerless attribution probe.
- CI rejects non-Release CMake caches before CMake-backed test or timing
  phases run.
- Docs record that the native-support page-publish skip prototype is not an
  accepted optimization because it failed throughput validation.

## Risks And Unresolved Questions

- Probe summaries make slow paths visible; they do not by themselves reduce
  per-process startup, mysqli connect, or ownerless autocommit cost.
- Reduced local probe runs are noisy. Optimization decisions still require
  repeated production samples with comparable storage placement and runner load.
- Broader native redo/checkpoint reconciliation remains the prerequisite before
  ownerless native-support page publication can be safely reduced.
