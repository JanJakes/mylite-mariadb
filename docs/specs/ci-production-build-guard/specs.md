# CI Production Build Guard

## Goal

Keep CI timing-sensitive jobs on production CMake artifacts. The ownerless
branch uses CI timings to compare startup, PHP extension, WordPress PHPUnit,
and embedded engine performance against trunk, so a hidden Debug or stale
non-Release build directory would make those timings unusable.

## Scope

This slice adds `tools/require-cmake-release-build`, a lightweight shell guard
that reads `CMAKE_BUILD_TYPE` from one or more `CMakeCache.txt` files and fails
unless each cache is `Release`.

CI runs the guard in all CMake-backed jobs:

- the normal build matrix after `cmake --preset prod`,
- the embedded job after `cmake --preset php-embedded-prod`,
- the WordPress PHPUnit job after the harness builds PHP extensions in
  `build/wordpress-php-embedded-prod`,
- the clang-tools job after `cmake --preset prod`.

The WordPress harness already enforces
`MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1` for CI phases. The explicit guard
gives the workflow a visible pass/fail line for the host-side generated cache
before dependency, database, performance-probe, and PHPUnit test-only phases
run.

## Non-Goals

- This does not change compiler flags or performance behavior.
- This does not make noisy CI timing comparable by itself; it removes build
  type drift as one variable.
- This does not change local compatibility presets that intentionally remain
  available for developer or hook builds.

## Verification

- `bash -n tools/require-cmake-release-build`
- `tools/require-cmake-release-build build/prod build/php-embedded-prod`
- `tools/require-cmake-release-build build/wordpress-php-embedded-prod`
- a temporary Debug `CMakeCache.txt` is rejected by the guard
- `cmake --build --preset format-check-prod`
- `git diff --check`
