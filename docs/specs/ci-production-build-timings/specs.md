# CI Production Build Timings

## Problem

The CI workflow separated embedded ownerless SQL timing and WordPress PHPUnit
phase timing, but the main CMake jobs still used `dev` and `php-embedded-dev`
presets. Those presets did not set `CMAKE_BUILD_TYPE` for the single-config
Ninja generator, so CI timing samples could be collected from unoptimized
builds. The WordPress PHPUnit harness also configured its CMake build inside
Docker without an explicit build type.

CI performance evidence is only useful for branch/main comparisons when the
jobs use production-style optimized binaries.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `CMakePresets.json` had `dev`, `embedded-dev`, and `php-embedded-dev`
  presets, all using the Ninja generator without `CMAKE_BUILD_TYPE`.
- `.github/workflows/ci.yml` used those dev presets for the build matrix,
  embedded tests, embedded performance probe, and clang-tidy compile database.
- `tools/wordpress-phpunit-mysqli-mylite` configured
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR` with `cmake -S . -B ... -G Ninja` and no
  `CMAKE_BUILD_TYPE`.
- Relative WordPress harness build-directory overrides were not normalized
  before the host-to-container path translation, so
  `build/wordpress-php-embedded-prod` mapped to `/workbuild/...` instead of
  `/work/build/...`.
- `tools/mariadb-embedded-build` already uses the MariaDB embedded baseline
  profile, and `cmake/mariadb-embedded-baseline.cmake` forces that upstream
  archive build to `MinSizeRel`. This slice therefore targeted MyLite's CMake
  build trees and PHP extension harness builds; a later guard slice added
  visible CI assertions for the separate MariaDB embedded archive caches.

## Design

Add production CMake presets:

- `prod`, inheriting `dev`, with `CMAKE_BUILD_TYPE=Release` and
  `build/prod`;
- `embedded-prod`, inheriting `prod`, with
  `MYLITE_WITH_MARIADB_EMBEDDED=ON` and `build/embedded-prod`;
- `php-embedded-prod`, inheriting `embedded-prod`, with
  `MYLITE_BUILD_PHP_EXTENSIONS=ON` and `build/php-embedded-prod`;
- matching build and test presets;
- `format-check-prod` and `tidy-prod` so the clang-tools CI job uses the same
  production compile database while preserving the normal developer presets.

Update CI so:

- the platform build matrix configures, builds, and tests `prod`;
- the embedded job configures/builds/tests `php-embedded-prod`;
- the direct ownerless SQL loop and embedded performance probe use
  `build/php-embedded-prod`;
- the test and performance-probe steps repeat the Release/MinSizeRel cache
  guards immediately before reporting timings, so a later stale or replaced
  build directory cannot silently produce comparable-looking CI output;
- the WordPress PHPUnit job sets
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod` and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`;
- clang-format checking and clang-tidy run from the production CMake preset.

Update the WordPress harness to default
`MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`, pass it through Docker, print it
in logs, and include `-DCMAKE_BUILD_TYPE` in the CMake configure command.
Normalize repository-local harness directories before mapping them into the
container so relative CI overrides remain under `/work/...`. Guarded phases now
also print the verified MyLite and MariaDB embedded CMake cache paths and build
types, and the perf-probe summary includes the requested build type plus the
process, connect, SQL, and write iteration counts used for the sample.

Release builds define `NDEBUG`, but the C test executables rely on `assert()`
for their checks. Keep MyLite libraries and PHP extensions as normal optimized
Release artifacts, and undefine `NDEBUG` only for first-party C test
executables so production CTest timing does not silently disable assertions.

## Compatibility Impact

No SQL, C API, PHP API, native storage, directory-layout, or ownerless
concurrency behavior changes. This slice changes build presets, CI commands,
and harness build configuration for timing fidelity.

## Directory And Lifecycle Impact

No durable database-directory changes. CI now writes production build artifacts
under separate build directories: `build/prod`, `build/embedded-prod`,
`build/php-embedded-prod`, and `build/wordpress-php-embedded-prod`.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage format changes. The MariaDB embedded archive remains built
through the existing `tools/mariadb-embedded-build` baseline profile.

## Build And Performance Impact

CI timings now represent optimized MyLite code. Developer presets remain
available for local edit/test loops. Production preset names make the build
mode explicit in logs and preserve separate build trees, avoiding accidental
reuse of unoptimized dev artifacts in timing-sensitive jobs.

The WordPress harness now defaults its CMake build to `Release`. Callers can
override this with `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE` when they intentionally
need a debug build.

The first Release build exposed two build-quality issues that are fixed in
this slice: a PDO SQLSTATE copy used `strncpy()` with a destination-sized
bound, and `tidy-prod` reported redundant anonymous-namespace `static`
definitions plus a manual max pattern in the page-log code.

## Test Plan

- Run `cmake --list-presets=all` and confirm the new production configure,
  build, and test presets are registered.
- Configure and build `prod`, then run `ctest --preset prod`.
- Configure and build `php-embedded-prod`.
- Run the production embedded performance probe.
- Run focused PHP CTest labels under `php-embedded-prod`.
- Run the CI-shaped embedded non-ownerless CTest command under
  `php-embedded-prod`.
- Run representative ownerless SQL cases under the production ownerless SQL
  binary.
- Run a reduced WordPress `build-php` phase with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` and confirm the logged build type
  plus generated wrapper artifacts.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `cmake --build --preset format-check-prod`.
- Run `cmake --build --preset tidy-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Every CI job that configures/builds/tests MyLite code uses an explicit
  production/Release build mode.
- Embedded performance probe timings come from `php-embedded-prod` binaries.
- WordPress PHPUnit and perf-probe timings come from a Release CMake build in
  `build/wordpress-php-embedded-prod`.
- CI timing and test steps repeat production cache guards immediately before
  running the timed command, not only after configure.
- WordPress timing logs show the verified CMake cache build type and sample
  iteration counts next to the summary metrics.
- Production CTest runs execute assertions even though the tested libraries are
  optimized Release artifacts.
- Developer `dev`, `embedded-dev`, and `php-embedded-dev` presets remain
  available unchanged for local development.

## Verification Results

- `cmake --list-presets=all`: passed; production configure/build/test presets,
  `format-check-prod`, and `tidy-prod` are registered.
- `cmake --preset prod`: passed with `CMAKE_BUILD_TYPE=Release`.
- `cmake --build --preset prod`: passed after keeping test assertions enabled
  under Release.
- `ctest --preset prod`: passed, 24/24 tests, 35.74s on the final rerun.
- `cmake --preset php-embedded-prod`: passed with
  `CMAKE_BUILD_TYPE=Release`, `MYLITE_WITH_MARIADB_EMBEDDED=ON`, and
  `MYLITE_BUILD_PHP_EXTENSIONS=ON`.
- `cmake --build --preset php-embedded-prod`: passed.
- Focused production embedded/PHP target build for
  `mylite_embedded_performance_probe`,
  `mylite_ownerless_cross_process_sql_test`, and the PHP extension modules:
  passed.
- `ctest --preset php-embedded-prod -L php --output-on-failure`: passed, 3/3
  tests, 8.81s.
- `ctest --preset php-embedded-prod -LE compat.ownerless-cross-process-sql
  --parallel 2 --output-on-failure`: passed, 48/48 tests, 56.15s.
- Production ownerless SQL binary `sql-case-count`: reported 161 cases.
- Representative ownerless production cases
  `prepared-committed-read`, `native-reclaim`, `live-reclaim`, `commit-race`,
  and `table-lock-wait-negative-proof`: passed.
- Production embedded performance probe: passed. Final sample showed ordinary
  autocommit inserts at 1767.65 ops/s and ownerless autocommit inserts at
  109.36 ops/s, while ownerless transactional inserts were 1377.21 ops/s.
- Reduced WordPress `build-php` phase with
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod` and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`: passed; the container mapped
  the build directory to `/work/build/wordpress-php-embedded-prod`,
  configured Release, and reported `wordpress_total_seconds=95`.
- `bash -n tools/wordpress-phpunit-mysqli-mylite`: passed.
- `cmake --build --preset format-check-prod`: passed.
- `cmake --build --preset tidy-prod`: passed after the page-log tidy cleanup.
- `git diff --check`: passed.
