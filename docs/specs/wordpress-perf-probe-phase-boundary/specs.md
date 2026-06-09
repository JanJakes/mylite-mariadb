# WordPress Perf Probe Phase Boundary

## Problem

The WordPress PHPUnit CI job runs production build/setup phases separately from
the timing-sensitive mysqli performance probe and PHPUnit test-only phases.
The `phpunit` phase already fails before test execution if the build,
dependency, or prepared database artifacts are missing. The `perf-probe` phase
validated the production build artifacts, but it could still connect to a fresh
MyLite path if the database preparation phase was skipped or the prepared
directory was deleted, folding setup work into a timing step.

## Source Findings

- `.github/workflows/ci.yml` runs the WordPress job with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`,
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`, and
  `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1`.
- The workflow separates Docker image build, WordPress fetch, PHP extension
  build, dependency installation, database preparation, mysqli performance
  probing, and PHPUnit shards into visible steps.
- `tools/wordpress-phpunit-mysqli-mylite` already makes the `phpunit` phase
  require build artifacts, WordPress/PHPUnit dependencies, `wp-tests-config.php`,
  and the prepared MyLite database directory.
- Before this slice, `MYLITE_WORDPRESS_PHASE=perf-probe` required only build
  artifacts before measuring process startup, connect/close, reads, and writes.

## Design

Make `perf-probe` call `require_prepared_database` before it starts measuring.
The prepared-database check requires both the WordPress test config and the
MyLite database directory created by `MYLITE_WORDPRESS_PHASE=prepare-db`.

This keeps the performance probe as a timing-only phase in CI and local audits:
missing setup now fails early with the same phase-boundary error style as
`phpunit`, rather than allowing a fresh database to be created in the measured
probe path.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or directory-layout
behavior changes. The harness phase contract is stricter for timing runs.

## Build And Performance Impact

No production library build impact. CI timing becomes more reliable because the
mysqli performance probe cannot silently include database setup after a skipped
or failed prepare step.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `MYLITE_WORDPRESS_PHASE=perf-probe` against an intentionally missing
  prepared tree and confirm it fails before measuring.
- Run production build guards for the local CI caches.
- Run a reduced production embedded performance probe to keep the current
  performance investigation grounded in optimized artifacts.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-09 used existing production caches:
`build/wordpress-php-embedded-prod` was `Release` and
`build/wordpress-mariadb-embedded` was `MinSizeRel`.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `git diff --check` passed.
- Production cache guards accepted `build/mariadb-embedded` as `MinSizeRel`
  and `build/prod`, `build/php-embedded-prod`,
  `build/wordpress-php-embedded-prod`, `build/ownerless-test-hooks`, and
  `build/ownerless-stress` as `Release`.
- A negative `MYLITE_WORDPRESS_PHASE=perf-probe` run with an intentionally
  missing `MYLITE_WORDPRESS_DB_DIR` exited with status `1` after printing
  `wordpress_cmake_cache_build_type=Release` and
  `wordpress_mariadb_embedded_cache_build_type=MinSizeRel`, then failed with
  `Missing prepared MyLite database directory` and instructed the caller to
  run `MYLITE_WORDPRESS_PHASE=prepare-db`.
- A reduced stats-off production embedded performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` passed. It reported ordinary
  autocommit inserts `1727.42 ops/s`, ownerless transactional inserts
  `1263.68 ops/s`, ownerless autocommit inserts `480.56 ops/s`, and
  ownerless active-runtime reconnect `1.034 ms` versus ordinary
  active-runtime reconnect `0.900 ms`.
- A reduced stats-enabled production embedded performance probe with
  `MYLITE_PERF_INSERT_ITERATIONS=200` and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. It kept the current
  ownerless autocommit attribution on optimized artifacts:
  `1.000` page-version record, `3.000` native-support pages elided,
  `1.000` snapshot-boundary page, `0.111 ms/insert` page-log append,
  `0.699 ms/insert` write-history cost, `2.000` exact history flush pages,
  and `0.000` exact-flush fallback rounds per insert.

## Acceptance Criteria

- WordPress `perf-probe` requires prepared database artifacts before timing.
- WordPress `phpunit` remains test-only.
- CI and local production timing comparisons still use Release MyLite PHP
  extensions and MinSizeRel MariaDB embedded archives.
