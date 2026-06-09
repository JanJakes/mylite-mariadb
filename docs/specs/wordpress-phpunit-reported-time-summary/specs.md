# WordPress PHPUnit Reported Time Summary

## Problem

The WordPress PHPUnit CI job now separates build, dependency, database
preparation, performance probing, and test-only phases. The remaining ambiguity
inside a test-only phase is that the shell wall time includes WordPress/PHPUnit
bootstrap and install work, while PHPUnit's own `Time:` line reports the test
body duration. Slow CI samples need both numbers in parseable form.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` runs WordPress PHPUnit through the
  `MYLITE_WORDPRESS_PHASE=phpunit` test-only phase after the production PHP
  extension build and database preparation steps.
- `tools/wordpress-phpunit-mysqli-mylite` already uses shell `time` to print
  `wordpress_phpunit_shell_real_seconds`, `wordpress_phpunit_shell_user_seconds`,
  and `wordpress_phpunit_shell_sys_seconds`.
- PHPUnit 9.6 prints a `Time:` line after the test body, for example
  `Time: 00:19.337, Memory: ...`.

## Design

Capture the streamed PHPUnit output to a temporary file while preserving normal
console output. After the command finishes, parse the final PHPUnit `Time:`
line and emit:

- `wordpress_phpunit_reported_seconds`, the PHPUnit-reported test body time;
- `wordpress_phpunit_shell_overhead_seconds`, shell real time minus reported
  PHPUnit time.

Keep the existing shell timing keys unchanged and continue to return PHPUnit's
actual exit status.

## Compatibility Impact

No SQL, PHP, mysqli, public C API, storage, or WordPress behavior changes. This
slice changes diagnostics emitted by the test harness only.

## Directory And Lifecycle Impact

No durable directory-layout change. The harness creates one temporary capture
file under `TMPDIR` and removes it before exiting the PHPUnit phase.

## Build And Performance Impact

No production build or binary-size impact. The added `tee`, `sed`, and `awk`
work runs after the PHPUnit process and is outside MyLite engine timings.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the `phpunit` phase with a focused `^Tests_DB::test_bail$` filter and
  confirm the reported-time and overhead keys are emitted.
- Run the production `^Tests_DB` shard and confirm the keys show the difference
  between PHPUnit test-body time and shell wall time.
- Run `git diff --check`.

## Acceptance Criteria

- The PHPUnit phase still streams output normally.
- The phase exits with PHPUnit's status.
- Successful PHPUnit runs emit `wordpress_phpunit_reported_seconds`.
- Runs with both shell and reported timing emit
  `wordpress_phpunit_shell_overhead_seconds`.
