# WordPress Timing Artifact Freshness

## Problem Statement

The WordPress timing harness already rejects non-Release MyLite CMake builds
and non-`MinSizeRel` MariaDB embedded archives. That is necessary but not
sufficient for trustworthy performance debugging: a local timing phase can use
a correctly configured build directory whose PHP extension artifacts are older
than the source files under test.

That stale-artifact path can produce exactly the wrong conclusion during a
performance investigation. A freshly edited `database.cc` can make the direct
embedded probe report current behavior while the WordPress Docker harness keeps
loading an older `mysqli_mylite.so`, or vice versa.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` runs the timing-producing
  WordPress `prepare-db`, `perf-probe`, and `phpunit` phases from the host
  wrapper, then starts Docker with the repository and build directory mounted
  under `/work`.
- The container-side `require_build_php_artifacts()` verifies that the PHP
  wrapper, `mylite.so`, `mysqli_mylite.so`, extension INI file, CMake build
  type, and MariaDB embedded build type exist and match the requested
  production profile.
- Those checks do not compare artifact mtimes against first-party source
  files. A local `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1` timing run can
  therefore reuse stale artifacts when the user forgot to rerun
  `MYLITE_WORDPRESS_PHASE=build-php`.
- `cmake/check-mariadb-embedded-freshness.cmake` already defines the
  authoritative MariaDB embedded archive freshness check by comparing
  `libmariadbd.a` against MariaDB source and
  `cmake/mariadb-embedded-baseline.cmake`.

## Design

Add a host-side freshness gate before Docker starts for WordPress phases that
consume existing PHP artifacts:

- `prepare-db`;
- `phpunit`;
- `perf-probe`.

The gate reuses `cmake/check-mariadb-embedded-freshness.cmake` for the
WordPress MariaDB embedded archive. It then checks the WordPress
`build-php` artifacts:

- `packages/libmylite/libmylite.a`;
- `packages/php-ext-mylite/mylite.so`;
- `packages/php-ext-mysqli-mylite/mysqli_mylite.so`.

Each artifact must be newer than its relevant first-party build inputs:

- `libmylite.a`: top-level/package CMake files, `cmake/*.cmake`,
  `packages/libmylite/include`, and `packages/libmylite/src`;
- `mylite.so`: top-level/package CMake files, `cmake/*.cmake`,
  `packages/php-ext-mylite/src`, and the just-checked `libmylite.a`;
- `mysqli_mylite.so`: top-level/package CMake files, `cmake/*.cmake`,
  public `libmylite` headers, and `packages/php-ext-mysqli-mylite/src`.

The mysqli adapter is not forced to rebuild for every `libmylite` source file
mtime because the WordPress wrapper loads `mylite.so` first and the adapter
uses the public MyLite header surface.

The check does not run for `build-php` because that phase creates the
artifacts. It also does not run for `all`, where the same container invocation
builds before using the artifacts.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, storage, or runtime behavior
changes. This only makes stale local timing runs fail early instead of
silently reporting outdated performance numbers.

## Directory And Lifecycle Impact

No durable files or database directory layout changes. The gate reads source
and build artifact metadata before Docker starts.

## Native Storage Impact

No native MariaDB or InnoDB storage behavior changes. The existing MariaDB
archive freshness check is reused.

## Build, Size, License, And Dependencies

No new dependencies or binary-size impact. The host wrapper already requires
standard shell tools and CMake for the WordPress production build flow.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a guarded WordPress `perf-probe` with the current stale WordPress PHP
  artifact and verify it fails before Docker starts with a stale-artifact
  message.
- Refresh the WordPress production PHP build with
  `MYLITE_WORDPRESS_PHASE=build-php`.
- Rerun a reduced guarded WordPress `perf-probe` and verify it passes, prints
  `wordpress_php_artifact_freshness_ok=...`, and appends that marker to the
  timing summary.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- WordPress timing phases fail before Docker timing starts when
  `libmylite.a`, `mylite.so`, or `mysqli_mylite.so` is older than relevant
  first-party source.
- The guard reuses the existing MariaDB embedded archive freshness check.
- `build-php` remains able to create or refresh artifacts.
- Passing timing phases emit an explicit freshness marker and record it in the
  timing summary.
- Production CI build-shape audits continue to pass.

## Verification Results

Completed.

## Evidence

The shell syntax check passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite
```

Before refreshing the WordPress PHP build, a guarded reduced `perf-probe`
failed before Docker timing started:

```text
MYLITE_WORDPRESS_PHASE=perf-probe \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-freshness-stale \
MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=1 \
MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS=1 \
MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=10 \
MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=2 \
tools/wordpress-phpunit-mysqli-mylite
```

The failure identified the stale artifact and the newer source:

```text
Stale WordPress libmylite archive: .../build/wordpress-php-embedded-prod/packages/libmylite/libmylite.a is older than .../packages/libmylite/src/database.cc.
Run MYLITE_WORDPRESS_PHASE=build-php before MYLITE_WORDPRESS_PHASE=perf-probe.
```

Refreshing the WordPress production PHP build succeeded:

```text
MYLITE_WORDPRESS_PHASE=build-php \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-freshness-build \
tools/wordpress-phpunit-mysqli-mylite
```

The build reused the existing `MinSizeRel` MariaDB embedded archive, rebuilt
`database.cc`, relinked `libmylite.a`, relinked `mylite.so`, and completed in
`32` seconds wall time.

After the refresh, the guarded reduced `perf-probe` passed and emitted the
freshness marker:

```text
MYLITE_WORDPRESS_PHASE=perf-probe \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-freshness-pass-summary \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-freshness/timing-summary.md \
MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=1 \
MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS=1 \
MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=10 \
MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=2 \
tools/wordpress-phpunit-mysqli-mylite
```

The pass case printed
`wordpress_php_artifact_freshness_ok=build/wordpress-php-embedded-prod`.
The timing summary also recorded:

```text
wordpress_php_artifact_freshness_ok=/home/agent/.paseo/worktrees/1irk9sr4/greasy-ostrich/build/wordpress-php-embedded-prod
wordpress_perf_summary_php_process_connect_close_ms_avg=659.545
wordpress_total_seconds=11
```

The production CI build-shape audit passed directly:

```text
tools/check-ci-production-builds
ci_production_build_audit_ok=/home/agent/.paseo/worktrees/1irk9sr4/greasy-ostrich/.github/workflows/ci.yml
```

The same audit passed through the production CTest preset:

```text
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
100% tests passed, 0 tests failed out of 1
```

The format and whitespace checks passed:

```text
cmake --build --preset format-check-prod
git diff --check
```

## Risks And Follow-Up

- This is a timing-evidence hardening slice, not a runtime optimization.
- The freshness check is mtime-based. It catches stale local artifacts after
  source edits, but it is not a reproducible-build hash verifier.
- The check is intentionally scoped to first-party MyLite/PHP extension inputs.
  Broader WordPress source or Composer dependency freshness remains owned by
  the existing `fetch` and `dependencies` phases.
