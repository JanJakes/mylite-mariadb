# WordPress PHPUnit Targeted Build

## Problem Statement

The WordPress mysqli PHPUnit harness now keeps the MyLite database on comparable
host-temp storage and ordinary MyLite SQL is back near the main runtime
baseline. The remaining CI/runtime complaint is total wrapper time: the harness
configured a broad CMake build and then built the default target set even though
the PHPUnit run only loads `mylite.so` and `mysqli_mylite.so`.

This slice narrows the harness build to the PHP modules required by WordPress so
the reported job time stays closer to the actual PHPUnit body.

## Source Findings

- `tools/wordpress-phpunit-mysqli-mylite` creates a PHP wrapper that loads only
  `packages/php-ext-mylite/mylite.so` and
  `packages/php-ext-mysqli-mylite/mysqli_mylite.so`.
- `packages/php-ext-mylite/CMakeLists.txt` defines target
  `mylite_php_extension`.
- `packages/php-ext-mysqli-mylite/CMakeLists.txt` defines target
  `mylite_mysqli_php_extension` and depends on `mylite_php_extension`.
- The prior harness ran `cmake --build <dir>` without `--target`, so Ninja could
  build unrelated PHP extension, test, and tool targets registered in the same
  build tree.

## Design

Configure the WordPress harness build with `BUILD_TESTING=OFF` and
`MYLITE_BUILD_TOOLS=OFF`, then build only the targets named by
`MYLITE_WORDPRESS_BUILD_TARGETS`. The default target list is:

- `mylite_php_extension`
- `mylite_mysqli_php_extension`

The environment override remains useful for local investigation, but CI and
normal local runs avoid compiling the PDO extension, embedded test executables,
and command-line tools before PHPUnit.

## Compatibility Impact

No SQL, PHP API, or MyLite runtime behavior changes. The WordPress harness still
loads the same modules and runs the same PHPUnit command.

## Directory And Lifecycle Impact

No MyLite database directory layout changes. The existing host-temp database
default remains unchanged.

## Build And Performance Impact

The build phase is narrower and should be less noisy after ownerless code
changes. Runtime comparisons should still use `wordpress_phpunit_seconds` and
PHPUnit's own elapsed time; total wrapper time now better reflects the required
module rebuild instead of unrelated targets.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a short WordPress harness filter when Docker is available.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The harness builds only the configured WordPress PHP module targets by
  default.
- The generated PHP wrapper still loads `mylite.so` and `mysqli_mylite.so`.
- The target list can be overridden for local diagnostics without changing the
  normal harness path.
