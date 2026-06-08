# CI Embedded Production Build Guard

## Problem

CI timing now separates build, setup, embedded performance probes, and
WordPress PHPUnit test-only phases. The MyLite CMake build trees are guarded as
`Release`, but the MariaDB embedded archive is built by
`tools/mariadb-embedded-build` outside those MyLite CMake caches. That archive
uses the documented production baseline `CMAKE_BUILD_TYPE=MinSizeRel`, so CI
needs a visible guard for it too. Otherwise a stale Debug MariaDB archive could
be linked into production MyLite or PHP-extension artifacts while later timing
steps only prove the outer MyLite cache is `Release`.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `cmake/mariadb-embedded-baseline.cmake` forces
  `CMAKE_BUILD_TYPE=MinSizeRel` for the embedded MariaDB archive.
- `.github/workflows/ci.yml` builds `build/mariadb-embedded` in the embedded
  job before configuring `php-embedded-prod`.
- `tools/wordpress-phpunit-mysqli-mylite` builds or reuses
  `build/wordpress-mariadb-embedded` before configuring the WordPress PHP
  extension build tree.
- Later WordPress CI phases run as test-only phases, so they need to reject a
  stale or mismatched MariaDB embedded cache before reporting performance or
  PHPUnit timings.

## Design

Add `tools/require-cmake-build-type`, an exact CMake cache build-type guard:

```sh
tools/require-cmake-build-type <expected-build-type> <cmake-build-dir>...
```

Keep the existing `tools/require-cmake-release-build` helper as the
Release-specific guard used by existing workflow steps.

Update CI so:

- the embedded job verifies `build/mariadb-embedded` is `MinSizeRel` after
  `tools/mariadb-embedded-build all`;
- the WordPress job verifies `build/wordpress-mariadb-embedded` is
  `MinSizeRel` after the `build-php` phase;
- the existing MyLite build-tree guards continue to verify `Release` for
  `build/prod`, `build/php-embedded-prod`, and
  `build/wordpress-php-embedded-prod`.

Update the WordPress harness so CI timing phases with
`MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1` also require the MariaDB embedded
archive cache to be `MinSizeRel`. This makes standalone `perf-probe` and
PHPUnit phases fail before they print timings if their linked embedded archive
is not the production baseline.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, native storage, metadata, or directory
lifecycle behavior changes. This slice only strengthens build and timing
guards.

## Build And Performance Impact

No compiler flags change. The guard reads `CMakeCache.txt` and exits before
timing-sensitive work when the cache type is wrong. CI timing evidence now
proves both sides of the linked embedded artifacts:

- MariaDB embedded archive: `MinSizeRel`;
- MyLite libraries, tests, and PHP extensions: `Release`.

## Verification Plan

- Run shell syntax checks for the guard scripts and WordPress harness.
- Verify the generic guard accepts local `MinSizeRel` MariaDB embedded caches.
- Verify the Release wrapper accepts local MyLite production caches.
- Verify a temporary Debug cache is rejected by the generic guard.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- CI has visible build-type guards for the MariaDB embedded archive in both
  embedded and WordPress timing jobs.
- WordPress test-only timing phases reject non-`MinSizeRel` embedded archive
  caches when CI release-build enforcement is enabled.
- Existing Release guards keep working for MyLite CMake build trees.
