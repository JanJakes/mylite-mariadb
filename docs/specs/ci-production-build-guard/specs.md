# CI Production Build Guard

## Goal

Keep CI timing-sensitive jobs on production CMake artifacts. The ownerless
branch uses CI timings to compare startup, PHP extension, WordPress PHPUnit,
and embedded engine performance against trunk, so a hidden Debug or stale
non-Release build directory would make those timings unusable.

## Scope

This slice adds `tools/require-cmake-release-build`, a lightweight shell guard
that reads `CMAKE_BUILD_TYPE` from one or more `CMakeCache.txt` files and fails
unless each cache is `Release`. A follow-up guard adds the generic
`tools/require-cmake-build-type` helper so CI can also assert the MariaDB
embedded archive's production `MinSizeRel` baseline.

CI runs the guard in all CMake-backed jobs:

- the normal build matrix after `cmake --preset prod`,
- the embedded job after `tools/mariadb-embedded-build all`, using the generic
  guard to require `MinSizeRel` for `build/mariadb-embedded`,
- the embedded job after `cmake --preset php-embedded-prod`,
- the WordPress PHPUnit job after the harness builds or reuses
  `build/wordpress-mariadb-embedded`, using the generic guard to require
  `MinSizeRel`,
- the WordPress PHPUnit job after the harness builds PHP extensions in
  `build/wordpress-php-embedded-prod`,
- the clang-tools job after `cmake --preset prod`.

The WordPress harness enforces `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1` for
CI phases. With that flag set it now requires the outer WordPress MyLite build
tree to be `Release` and the linked MariaDB embedded archive to be
`MinSizeRel`. The explicit workflow guards give the workflow visible pass/fail
lines for both host-side generated caches before dependency, database,
performance-probe, and PHPUnit test-only phases run.

## Non-Goals

- This does not change compiler flags or performance behavior.
- This does not make noisy CI timing comparable by itself; it removes build
  type drift as one variable.
- This does not change local compatibility presets that intentionally remain
  available for developer or hook builds.

## Verification

- `bash -n tools/require-cmake-release-build`
- `bash -n tools/require-cmake-build-type`
- `tools/require-cmake-release-build build/prod build/php-embedded-prod`
- `tools/require-cmake-release-build build/wordpress-php-embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-build-type MinSizeRel build/wordpress-mariadb-embedded`
- a temporary Debug `CMakeCache.txt` is rejected by the guard
- `cmake --build --preset format-check-prod`
- `git diff --check`
