# WordPress PHPUnit JUnit Slow Report

## Problem

The production WordPress PHPUnit job now exposes build, setup, performance
probe, and test-only phases separately, but the largest test-only bucket can
still be opaque. The latest production non-isolated remaining shard passed
28,687 tests in `2757.813s` shell real, but CI logs did not identify which
classes or methods consumed that time.

The harness already had opt-in JUnit logging through
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`. CI kept it disabled to avoid adding
per-test report work before production build parity was established. Now that
CI timing is production-guarded and the process-isolated shards are narrowed to
exact methods, JUnit timing is the next low-risk way to make PHPUnit
performance work actionable.

## Non-Goals

- Do not change SQL, mysqli, PHP extension, or storage-engine behavior.
- Do not claim the WordPress suite is faster.
- Do not upload or persist JUnit artifacts in this slice; print the slowest
  timing rows directly into the job log first.
- Do not make JUnit parsing run when a caller supplies its own JUnit path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not change
  MariaDB source.
- `tools/wordpress-phpunit-mysqli-mylite` already forwards
  `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT` into the Docker container and appends
  `--log-junit <report-dir>/phpunit-junit.xml` when the caller has not supplied
  a JUnit argument.
- The same harness already captures PHPUnit output to compute
  `wordpress_phpunit_reported_seconds` and shell overhead, so the JUnit report
  can be summarized in the same phase without changing test selection.
- `.github/workflows/ci.yml` owns the production WordPress job environment, and
  `tools/check-ci-production-builds` owns workflow timing invariants.

## Compatibility Impact

No MySQL/MariaDB compatibility surface changes. This is CI and harness
diagnostic output only.

## Design

When `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` and the harness created the JUnit
path itself, parse the generated XML after PHPUnit exits and print:

- `wordpress_phpunit_slow_report_path`;
- top 20 classes by cumulative testcase time, with rank, seconds, test count,
  and class name;
- top 20 methods by testcase time, with rank, seconds, class name, and method
  name.

Use the PHP wrapper already built for the WordPress job to parse the XML inside
the same container environment. This avoids new host dependencies and keeps the
logic available anywhere the harness already runs.

Enable `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` in the production WordPress CI
job and require that marker from `tools/check-ci-production-builds`, so future
workflow edits cannot remove the slow-test report silently.

## File Lifecycle

No MyLite database-directory changes. The JUnit XML remains in the existing
WordPress PHPUnit report directory under the build tree and may be overwritten
by later PHPUnit phases, matching the previous opt-in JUnit behavior.

## Embedded Lifecycle And API

No public API or embedded runtime lifecycle changes. The parser runs only after
PHPUnit returns and does not open MyLite itself.

## Build, Size, And Dependencies

No production binary, compiler flag, or third-party dependency changes. JUnit
summary parsing uses PHP's built-in XML support in the existing WordPress
Docker/PHP environment.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest audit:
  `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run a focused production WordPress PHPUnit shard with
  `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` and confirm the slow class/method
  lines print.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Production WordPress CI enables harness-owned JUnit logging.
- The CI production-build audit requires the JUnit setting.
- Focused production PHPUnit output includes slowest class and method timing
  rows.
- PHPUnit pass/fail behavior remains controlled by PHPUnit's own exit status.

## Verification Results

Local verification on 2026-06-10 used the production
`build/wordpress-php-embedded-prod` Release build, the
`build/wordpress-mariadb-embedded` MinSizeRel archive, the CI-pinned WordPress
ref `6ddfc9d9b532c6e95c1266165149815895e2eb56`, and the warmed WordPress
Docker image.

- `bash -n tools/wordpress-phpunit-mysqli-mylite`: passed.
- `bash -n tools/check-ci-production-builds`: passed.
- `tools/check-ci-production-builds`: passed and reported
  `ci_production_build_audit_ok`.
- Focused production WordPress PHPUnit with
  `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` and `--filter '^Tests_DB'` passed
  651 tests with 983 assertions and 3 skipped. It reported PHPUnit
  `Time: 00:15.925`, `wordpress_phpunit_shell_real_seconds=34.050`, and
  `wordpress_phpunit_shell_overhead_seconds=18.125`.
- The same focused run printed the new slow report. The top classes were
  `Tests_DB` at `9.827s` over 508 tests, `Tests_DB_Charset` at `2.445s` over
  100 tests, and `Tests_DB_dbDelta` at `1.612s` over 31 tests. The top methods
  were `Tests_DB::test_db_reconnect` at `0.460s`,
  `Tests_DB::test_mysqli_flush_sync` at `0.282s`, and
  `Tests_DB_dbDelta::test_wp_get_db_schema_does_not_alter_queries_on_existing_install`
  at `0.149s`.
- After adding stale JUnit cleanup before each harness-owned report path, a
  focused production run with `--filter 'Tests_DB::test_db_reconnect'` passed
  1 test with 2 assertions and printed a fresh slow report with `Tests_DB` and
  `test_db_reconnect` as the sole class and method rows.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed, 1/1 tests, `0.57 sec`.
- Production build guards passed for `build/prod`, `build/php-embedded-prod`,
  `build/wordpress-php-embedded-prod`, `build/mariadb-embedded`, and
  `build/wordpress-mariadb-embedded`.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Risks And Open Questions

- The report adds XML generation and parsing overhead to CI. The expected
  overhead is small relative to the current 45-minute non-isolated shard, but
  CI logs should be watched for a measurable increase.
- If the report shows most time in a few WordPress classes, a later slice can
  split or optimize those classes. If time is broadly distributed, wall-clock
  reduction likely requires parallel CI sharding rather than engine changes.
