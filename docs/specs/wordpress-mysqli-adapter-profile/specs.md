# WordPress MySQLi Adapter Profile

## Problem

The WordPress PHPUnit CI job now runs production builds and splits build,
dependency, database-prep, performance-probe, and PHPUnit test-only phases.
The current green production run shows the long pole is the non-isolated
PHPUnit body, not setup: the non-isolated step reported
`wordpress_phpunit_reported_seconds=1418.425` and
`wordpress_phpunit_shell_real_seconds=1422.959`.

The existing WordPress mysqli performance probe separates PHP process startup,
extension load, full process plus connect/close, in-process connect/close,
active-runtime reconnect, and small SQL loops. It does not show what the full
non-isolated PHPUnit step did at the mysqli adapter boundary. Without that
attribution, an optimization could target embedded open/close when the suite is
actually dominated by result queries, prepared statements, or direct DML.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/api/php-extensions.md` defines `mysqli_mylite` as an adapter extension
  that depends on the loaded `mylite` extension and exposes a MyLite-backed
  mysqli-shaped API. The mysqli host argument is interpreted as the MyLite
  database directory path, not a network server.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` centralizes:
  - `php_mylite_mysqli_open_link()` for `mylite_open()`,
  - `php_mylite_mysqli_query_impl()` for direct `mysqli_query()` calls,
  - `php_mylite_mysqli_exec_no_result_query_impl()` for no-result DML/DDL fast
    path execution through `mylite_exec()`,
  - `php_mylite_mysqli_execute_result_stmt()` for result-query execution
    through a cached `mylite_stmt`,
  - `php_mylite_mysqli_prepare_impl()` and
    `php_mylite_mysqli_stmt_execute_impl()` for explicit prepared statements,
  - object destructors and `mysqli_close()` for `mylite_close()`.
- `.github/workflows/ci.yml` already splits WordPress phases and runs the
  timing-sensitive WordPress steps with `Release` MyLite and `MinSizeRel`
  MariaDB embedded guards.
- The latest green production CI run for this branch reported the WordPress
  performance probe with stock PHP startup `16.975 ms`, MyLite extension
  process overhead `4.647 ms`, process plus MyLite connect/close `325.752 ms`,
  in-process mysqli connect/close `295.387 ms`, active-runtime reconnect
  `1.905 ms`, direct `SELECT 1` `823.84 ops/s`, transactional inserts
  `1518.68 ops/s`, point selects `514.72 ops/s`, and direct autocommit inserts
  `1259.48 ops/s`.

## Design

Add an opt-in profile mode to `mysqli_mylite`, enabled only when
`MYLITE_MYSQLI_PROFILE=1` is present in the PHP process environment. The
extension reads that environment once during module initialization.

When enabled, the adapter counts and times:

- `mylite_open()` and `mylite_close()` calls, split by explicit close,
  object-free close, and reconnect close,
- direct `mysqli_query()` calls, split into result-query, no-result
  DML/DDL, and `CALL` execution paths,
- result-query cache hits, misses, reset failures, prepare time, execution
  time, and rows returned,
- `mylite_exec()` time for result and no-result direct query paths,
- explicit `mysqli_prepare()` calls,
- `mysqli_stmt_execute()` calls, reset/bind/step subphase time, and prepared
  rows returned,
- result fetch helper call counts.

The counters are process-local diagnostics. They emit `mylite_mysqli_profile_*`
key/value lines at PHP module shutdown when profiling was enabled and at least
one profiled operation ran. Disabled mode adds only a cheap branch around
profile recording sites.

Expose the mode through the WordPress harness as
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, which sets
`MYLITE_MYSQLI_PROFILE=1` only for the PHPUnit process. CI enables it only for
the non-isolated remaining suite, because that is the slow production step. The
database, process-isolated, and performance-probe steps stay unprofiled unless
an explicit diagnostic run opts in.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or WordPress behavior changes. The
profile mode only emits diagnostic log lines when explicitly enabled through an
environment variable.

## Directory And Lifecycle Impact

No durable directory-layout change. The profile records in-process counters and
does not create files. It observes `mylite_open()` and `mylite_close()` calls
without changing handle ownership or close semantics.

## Build, Size, And Dependency Impact

The change adds small first-party instrumentation to the mysqli adapter and no
new dependencies. Production binaries include the dormant counters, but normal
execution leaves timing disabled.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build `mylite_mysqli_php_extension` with the production PHP embedded preset.
- Run the PHP mysqli adapter tests, including the profile-emission test.
- Run a reduced production WordPress PHPUnit `^Tests_DB` sample with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` and verify the profile lines
  include open, close, query, and prepared-statement counters.
- Run `tools/check-ci-production-builds` and the production CTest audit so CI
  keeps production build guards around timing steps.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Profiling is disabled unless `MYLITE_MYSQLI_PROFILE=1` is set.
- Enabled profiling prints parseable `mylite_mysqli_profile_*` lines at process
  shutdown.
- CI keeps profile-heavy mysqli attribution off the normal long PHPUnit timing
  step; explicitly profiled diagnostic phases still keep `Release`/`MinSizeRel`
  guards in their step.
- The emitted counters can distinguish lifecycle churn from direct query,
  result-query, and prepared-statement adapter work.
- Existing PHP mysqli compatibility tests still pass.

## Verification Results

Local verification on 2026-06-11 used production build caches:
`build/php-embedded-prod` with `Release` MyLite, and the WordPress harness
using `build/wordpress-php-embedded-prod` `Release`,
`build/wordpress-mariadb-embedded` `MinSizeRel`, the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, and a `/tmp`-mounted WordPress
MyLite test database outside the repository worktree.

- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension -j 4` passed.
- `ctest --preset php-embedded-prod -R
  'php-ext-mysqli-mylite\.(api|profile)$' --output-on-failure` passed.
- `ctest --preset php-embedded-prod -L php --output-on-failure` passed `4/4`
  PHP integration tests.
- `MYLITE_WORDPRESS_PHASE=build-php ... tools/wordpress-phpunit-mysqli-mylite`
  rebuilt the WordPress production PHP extension artifacts after rebuilding the
  `MinSizeRel` MariaDB embedded archive; the phase reported
  `mylite_build_seconds=100`.
- `MYLITE_WORDPRESS_PHASE=prepare-db ... tools/wordpress-phpunit-mysqli-mylite`
  passed with `wordpress_prepare_db_seconds=3`.
- A focused production WordPress PHPUnit sample with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` and `--filter '^Tests_DB'`
  passed `651` tests with `3` skips. The final PHPUnit process reported
  `wordpress_phpunit_reported_seconds=17.285`,
  `wordpress_phpunit_shell_real_seconds=38.309`, `open_calls=10`,
  `open_ms_total=382.096`, `close_calls=10`,
  `close_ms_total=1010.412`, `query_calls=4010`,
  `query_ms_total=12664.503`, `query_result_calls=1615`,
  `query_prepare_ms_total=2890.317`,
  `query_result_execute_ms_total=3192.299`,
  `exec_no_result_calls=2394`, `exec_no_result_ms_total=4074.160`,
  `query_result_rows=75033`, and `fetch_object_calls=76626`.
- The same focused sample also emitted an earlier WordPress install process
  summary with one full open/close and `349` adapter queries, confirming
  process-local summaries are visible for each profiled PHP process.
- A production WordPress mysqli `perf-probe` with CI-sized iteration counts
  passed after the production rebuild. On this loaded local host it reported
  process-plus-connect/close `650.041 ms`, in-process connect/close
  `531.940 ms`, active-runtime reconnect `5.794 ms`, direct `SELECT 1`
  `351.53 ops/s`, transactional inserts `674.33 ops/s`, point selects
  `247.07 ops/s`, prepared autocommit inserts `771.24 ops/s`, and direct
  autocommit inserts `751.59 ops/s`.
- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Open Questions

- The profile measures adapter-boundary time, not every MariaDB internal
  subphase. If the next signal points at engine execution, a lower-level
  `libmylite` or MariaDB attribution slice is still needed.
- PHP module-shutdown output is process-local. If a future PHPUnit partition
  includes child processes while profiling is enabled, each child may emit its
  own process summary.
