# WordPress Production Build Guard

## Problem

The CI workflow now splits WordPress PHPUnit work into visible setup, build,
dependency, database preparation, performance-probe, and test-only steps, and the
job requests a Release build in `build/wordpress-php-embedded-prod`. The
test-only and performance-probe phases only checked for existing artifacts,
though. A stale build directory with a mismatched CMake build type could produce
valid-looking timings from non-production artifacts.

## Source Findings

- `.github/workflows/ci.yml` uses production presets for the first-party build
  matrix, embedded tests, ownerless SQL tests, embedded performance probe, and
  clang-tool checks.
- The WordPress job sets `MYLITE_WORDPRESS_CMAKE_BUILD_DIR` to
  `build/wordpress-php-embedded-prod` and `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE` to
  `Release`.
- `tools/wordpress-phpunit-mysqli-mylite` configures the PHP-extension build
  with `-DCMAKE_BUILD_TYPE="${MYLITE_WORDPRESS_CMAKE_BUILD_TYPE}"` and later
  phases verify that the wrapper and extensions exist before running database
  preparation, `perf-probe`, or `phpunit`.
- The previous artifact check did not read `CMakeCache.txt`, so it could not
  prove that later timing phases were using artifacts built with the requested
  production build type.

## Design

Add a `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD` guard to the WordPress harness.
When enabled, the harness:

- rejects any requested build type other than `Release`;
- verifies that `${MYLITE_WORDPRESS_CMAKE_BUILD_DIR}/CMakeCache.txt` exists;
- checks that the cached `CMAKE_BUILD_TYPE` matches the requested build type;
- rejects any cached build type other than `Release`;
- applies the same guard during `build-php`, `prepare-db`, `perf-probe`, and
  `phpunit` phases.

Enable the guard in the CI WordPress job. Local callers keep the existing
default behavior unless they opt into the guard, while the default local build
type remains `Release`.

## Compatibility Impact

No SQL, PHP API, mysqli, C API, storage-engine, or ownerless behavior changes.
This only hardens CI and harness timing validation.

## Directory And Lifecycle Impact

No durable database-directory layout changes. The guard reads the CMake cache in
the existing build directory and does not touch the MyLite database directory.

## Native Storage Impact

No native storage format or recovery behavior changes.

## Build And Performance Impact

CI timing-sensitive WordPress phases now fail fast instead of reporting timings
from non-Release artifacts. The guard adds a small shell `sed` read outside the
measured PHPUnit and performance loops. Production CMake presets and build
targets remain unchanged.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a no-Docker syntax/argument smoke by setting
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=bad` and expecting the host wrapper to
  reject it.
- Run `MYLITE_WORDPRESS_PHASE=build-php` with
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`, and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` on a warmed Docker/build tree.
- Run reduced production WordPress `perf-probe` and focused `Tests_DB` if the
  warmed Docker image, WordPress checkout, dependencies, and prepared database
  are available.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- CI explicitly opts into the Release-build guard for the WordPress timing job.
- The `build-php` phase rejects a non-Release request when the guard is enabled.
- Later `prepare-db`, `perf-probe`, and `phpunit` phases reject missing,
  mismatched, or non-Release CMake caches instead of silently timing stale
  artifacts.
- Existing default local harness behavior remains available.

## Verification Results

Local verification on 2026-06-08 used the warmed WordPress Docker image, pinned
CI WordPress ref `6ddfc9d9b532c6e95c1266165149815895e2eb56`, production
`build/wordpress-php-embedded-prod` CMake build directory, and the prepared
`/tmp` WordPress MyLite database.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=bad
  tools/wordpress-phpunit-mysqli-mylite` rejected the invalid guard value before
  starting Docker.
- Guarded production `build-php` passed with
  `wordpress_require_release_build=1`,
  `mylite_mariadb_embedded_seconds=96`, `mylite_php_configure_seconds=1`,
  `mylite_php_build_seconds=25`, and `wordpress_total_seconds=130`; the embedded
  MariaDB archive reported the optimized `CMAKE_BUILD_TYPE:STRING=MinSizeRel`
  baseline, while the PHP-extension CMake cache was `Release`.
- Guarded reduced production `perf-probe` passed with one process/connect
  iteration, five SQL iterations, and two write iterations, printing
  `wordpress_perf_summary_*` lines including process startup, process/connect
  delta, active-runtime reconnect, read throughput, and write throughput.
- Guarded `MYLITE_WORDPRESS_PHASE=phpunit --filter Tests_DB` passed 651 tests
  with 3 skips; PHPUnit reported `00:20.556`, the shell timer reported
  `wordpress_phpunit_shell_real_seconds=48.819`, and the phase reported
  `wordpress_total_seconds=53`.
- Guarded `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Debug
  MYLITE_WORDPRESS_PHASE=phpunit --filter Tests_DB` rejected the non-Release
  request before invoking PHPUnit.

## Risks And Unresolved Questions

- The guard proves CMake build type, not CPU load or filesystem placement.
  Branch/main performance comparisons still require comparable runner/storage
  conditions.
- The MariaDB embedded archive remains governed by
  `cmake/mariadb-embedded-baseline.cmake`; this slice does not change that
  optimized baseline or add a separate cache guard for it.
