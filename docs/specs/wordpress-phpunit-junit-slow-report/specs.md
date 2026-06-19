# WordPress PHPUnit JUnit Slow Report

## Problem

The production WordPress PHPUnit job now exposes build, setup, performance
probe, and test-only phases separately, but the largest test-only bucket can
still be opaque. The latest production non-isolated remaining shard passed
28,687 tests in `2757.813s` shell real, but CI logs did not identify which
classes or methods consumed that time.

The harness already had opt-in JUnit logging through
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`. The critical CI timing path keeps it
disabled because full-suite XML logging changes the measured test cost. The
diagnostic path still needs enough output to decide whether a slow shard is
dominated by a few classes or broadly distributed across WordPress tests.

## Non-Goals

- Do not change SQL, mysqli, PHP extension, or storage-engine behavior.
- Do not claim the WordPress suite is faster.
- Do not upload or persist JUnit artifacts in this slice; print diagnostic rows
  into the job log and append compact aggregate rows to the timing summary.
- Do not make JUnit parsing run when a caller supplies its own JUnit path.
- Do not enable JUnit on the default production CI timing path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not change
  MariaDB source.
- `tools/wordpress-phpunit-mysqli-mylite` forwards
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
- aggregate distribution rows for testcase count, class count, total testcase
  time, top-class time, selected top-class time, and selected top-class ratio;
- top classes by cumulative testcase time, with rank, seconds, test count, and
  class name;
- top methods by testcase time, with rank, seconds, class name, and method name.

`MYLITE_WORDPRESS_PHPUNIT_SLOW_REPORT_LIMIT` selects the number of class and
method rows and defaults to `20`. The harness validates it as a positive
integer on the host before entering the Docker container, forwards it, and uses
the same limit inside the XML parser. Compact aggregate rows are appended to
the timing summary so diagnostic runs can tell whether a shard is concentrated
or broad without scraping the full class list.

Use the PHP wrapper already built for the WordPress job to parse the XML inside
the same container environment. This avoids new host dependencies and keeps the
logic available anywhere the harness already runs.

Keep `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=0` and
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1` on the default production WordPress CI
timing path. Diagnostic runs that need per-test attribution opt into
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` and set
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=0`.

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
  `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`,
  `MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=0`, and a non-default
  `MYLITE_WORDPRESS_PHPUNIT_SLOW_REPORT_LIMIT`; confirm the aggregate
  distribution rows and bounded slow class/method lines print.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Production WordPress CI keeps harness-owned JUnit logging disabled on the
  critical timing path.
- Focused production PHPUnit diagnostic output includes slow-report aggregate
  rows and bounded slowest class/method timing rows.
- Diagnostic slow-report aggregate rows are appended to the WordPress timing
  summary.
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

Follow-up diagnostic verification on 2026-06-19 refreshed the production
WordPress PHP build after the MariaDB embedded archive freshness check reported
`libmariadbd.a` older than `mariadb/sql/handler.cc`. The refreshed archive used
the `build/wordpress-mariadb-embedded` `MinSizeRel` profile and the PHP build
used `build/wordpress-php-embedded-prod` `Release`.

- The CI-shaped `prepare-db` phase passed with the external tmpfs-backed
  WordPress MyLite test database.
- A first diagnostic run proved malformed ad hoc filters can select zero tests,
  but still printed the JUnit slow-report headers without changing PHPUnit exit
  handling.
- The corrected CI-filtered non-isolated remaining diagnostic run passed
  16,811 tests with 3,384,801 assertions, 77 upstream PHPUnit warnings, and
  79 skipped tests. It reported `wordpress_phpunit_shell_real_seconds=508.734`,
  `wordpress_phpunit_reported_seconds=500.281`, and
  `wordpress_phpunit_shell_overhead_seconds=8.453`.
- The generated JUnit XML contained 16,809 testcase entries across 845 classes,
  `417.660s` of summed testcase time, `113.720s` in the top 20 classes, and a
  top-20 ratio of `0.2723`. The top classes were
  `Tests_Term_getTerms` at `12.606s`, `Tests_Media` at `11.685s`, and
  `Tests_User_Capabilities` at `8.597s`, proving the shard is broad rather
  than dominated by one pathological class.
- After adding aggregate output and a configurable report limit, a focused
  production-shaped diagnostic run with
  `MYLITE_WORDPRESS_PHPUNIT_SLOW_REPORT_LIMIT=3` and
  `--filter 'Tests_DB::test_db_reconnect'` passed 1 test with 2 assertions,
  reported `wordpress_phpunit_slow_report_testcase_count=1`,
  `wordpress_phpunit_slow_report_class_count=1`,
  `wordpress_phpunit_slow_report_total_case_time_seconds=0.198`,
  `wordpress_phpunit_slow_report_top_classes_time_ratio=1.0000`, and bounded
  the slowest class/method limits to `3`. The same aggregate rows were appended
  under the `manual-focused-slow-report-limit` timing-summary label.
- `MYLITE_WORDPRESS_PHPUNIT_SLOW_REPORT_LIMIT=0` failed before Docker startup
  with `MYLITE_WORDPRESS_PHPUNIT_SLOW_REPORT_LIMIT must be a positive integer`.

## Risks And Open Questions

- The report adds XML generation and parsing overhead. The 2026-06-19
  diagnostic run was materially slower than the no-logging CI timing path, so
  JUnit remains opt-in rather than enabled on critical CI timings.
- If the report shows most time in a few WordPress classes, a later slice can
  split or optimize those classes. If time is broadly distributed, wall-clock
  reduction likely requires parallel CI sharding rather than engine changes.
