# WordPress PHPUnit Suite Split Profile

## Problem

The WordPress PHPUnit CI job already separates Docker image build, WordPress
fetch, MyLite PHP extension build, dependency installation, database
preparation, performance probing, and PHPUnit execution. The remaining
visibility gap is inside the PHPUnit execution step itself: the full suite
mixes the database-focused `Tests_DB` class family with the rest of WordPress'
test corpus, so a slow CI sample does not immediately show whether the MyLite
mysqli path, PHP process churn, or non-database WordPress tests moved.

The performance question for this branch is also narrower than total job wall
time. WordPress uses the ordinary embedded mysqli path, while ownerless
cross-process mode is only active for `MYLITE_OPEN_OWNERLESS_RW`. The branch
therefore needs separate evidence for:

- PHP process startup and extension-load cost,
- short-lived PHP process plus MyLite connect/close cost,
- in-process ordinary mysqli engine throughput,
- lower-level ordinary and ownerless C API throughput.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current branch head for this slice: `56a8b629`.
- Current `origin/main`: `4760d512`, the same pinned main baseline used by the
  prior WordPress PHPUnit performance docs.
- `.github/workflows/ci.yml` runs the WordPress job through
  `tools/wordpress-phpunit-mysqli-mylite` phases. The `phpunit` phase forwards
  caller arguments directly to PHPUnit.
- WordPress' pinned PHPUnit tree names the database-focused class family with
  the `Tests_DB` prefix, including `Tests_DB` and related `Tests_DB_*` classes.
- PHPUnit 9.6 accepts regex filters for execution, but `--list-tests` ignores
  `--filter`, so filter validation must use real short test executions rather
  than listing mode.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens WordPress
  mysqli connections with `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE`, not
  `MYLITE_OPEN_OWNERLESS_RW`. Ownerless page-log, page-publish, and
  cross-process lock machinery should therefore not run on the ordinary
  WordPress hot path.

## Design

Split the CI PHPUnit execution into two serial steps:

- `Run WordPress PHPUnit database suite` executes
  `tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB'`.
- `Run WordPress PHPUnit remaining suite` executes
  `tools/wordpress-phpunit-mysqli-mylite --filter '^(?!Tests_DB)'`.

The steps remain after database preparation and the WordPress mysqli
performance probe. They are intentionally serial and reuse the prepared
WordPress MyLite database directory. This split is for visible CI timings, not
parallel execution; concurrent WordPress harness processes against the same
ordinary MyLite database directory can conflict.

The follow-up timing visibility change makes the `phpunit` phase fail early if
the WordPress checkout, PHP extension build artifacts, Composer/PHPUnit
dependencies, `wp-tests-config.php`, or prepared MyLite database directory are
missing. The PHPUnit CI steps are labelled as test-only steps, and the harness
does not fall back to build, fetch, dependency, or database-preparation work
inside the `phpunit` phase.
The mysqli `perf-probe` phase now uses the same prepared-database requirement
before measuring process/connect and SQL-loop timings, so skipped setup cannot
create a fresh database inside a timing step.

The CI `perf-probe` phase uses five process/connect samples, matching the
harness default, so PHP startup, extension-load, process plus connect/close,
in-process connect/close, and active-runtime reconnect averages are less
sensitive to a single slow child process while SQL/read/write loop counts stay
bounded for CI.

No harness default changes are needed. Local callers that run
`tools/wordpress-phpunit-mysqli-mylite` without PHPUnit filters still execute
the same full suite as before.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or native storage behavior changes.
The CI workflow still runs the same WordPress PHPUnit corpus, now partitioned
by class-name prefix.

## Directory And Lifecycle Impact

No durable directory-layout changes. The split suite steps use the same
prepared WordPress MyLite database directory lifecycle as the previous single
suite step.

## Performance Findings

Fresh local profiling on 2026-06-08 used the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, the current ownerless branch head
`56a8b629`, the existing Docker image, warmed build trees, and a WordPress
database on host `/tmp` mounted into the container.

The WordPress mysqli `perf-probe` reported:

- stock PHP process startup: `58.084ms`,
- PHP wrapper startup with MyLite extensions loaded: `78.365ms`,
- PHP process plus MyLite connect/close: `760.817ms`,
- in-process mysqli connect/close: `486.027ms`,
- `SELECT 1`: `257.73 ops/s`,
- transactional prepared inserts: `417.21 ops/s`,
- primary-key point selects: `259.74 ops/s`,
- prepared autocommit inserts: `369.57 ops/s`,
- direct-string autocommit inserts: `769.93 ops/s`.

The lower-level embedded C API performance probe with ownerless page-publish
stats enabled reported:

- ordinary cold create/open/close: `925.335ms`,
- ordinary warm open/close: `349.703ms`,
- ownerless warm open/close: `401.081ms`,
- ordinary direct `SELECT 1`: `4198.22 ops/s`,
- ordinary prepared `SELECT 1`: `2225.61 ops/s`,
- ordinary transactional inserts: `2132.92 ops/s`,
- ordinary autocommit inserts: `2123.68 ops/s`,
- ownerless direct `SELECT 1`: `3233.44 ops/s`,
- ownerless prepared `SELECT 1`: `2058.98 ops/s`,
- ownerless transactional inserts: `972.93 ops/s`,
- ownerless autocommit inserts: `82.28 ops/s`.

The same probe shows the remaining ownerless autocommit cost is concentrated in
ownerless-only native visibility work, not the ordinary WordPress mysqli path:

- `1600` autocommit page versions published for `200` inserts,
- page-publish hook total: `153.329ms`, including `138.955ms` in append work,
- page-read total: `309.713ms`, with `5776` WAL scans and `10170` negative
  cache hits,
- page-write enter total: `213.936ms`,
- page-write refresh total: `191.781ms`,
- page-write publish total: `161.699ms`.

The split `^Tests_DB` WordPress PHPUnit step completed `651` tests with `3`
skips, PHPUnit `Time: 00:17.172`, `wordpress_phpunit_shell_real_seconds=27.960`,
and `wordpress_phpunit_seconds=28`.

A fresh same-machine reference run from `/tmp/mylite-mariadb-reference` at
`origin/main` `4760d512` used the same pinned WordPress ref and `^Tests_DB`
filter. Main reported PHPUnit `Time: 00:23.168`,
`wordpress_phpunit_seconds=39`, and `wordpress_total_seconds=437`. The large
total includes main's old cold MariaDB embedded archive build and dependency
setup path; the PHPUnit body itself is slower than the current branch sample on
this run.

Filter validation used real short PHPUnit executions because listing mode
ignores filters:

- `--filter '^Tests_DB::test_bail$'` ran one `Tests_DB` test successfully.
- `--filter '^(?!Tests_DB).*Tests_Actions::test_simple_action$'` ran one
  non-DB test successfully.
- `--filter '^(?!Tests_DB).*Tests_DB::test_bail$'` executed no tests,
  confirming the negative lookahead excludes the DB class family.

A follow-up audit on 2026-06-10 found a later workflow filter had accidentally
narrowed that boundary to an exact `Tests_DB` class exclusion while adding
process-isolated class and method exclusions. That shape rejected
`Tests_DB::test_bail` but still matched `Tests_DB_Charset`,
`Tests_DB_dbDelta`, and `Tests_DB_RealEscape`. The workflow has been restored
to a leading `^(?!Tests_DB)` exclusion, and the production-build audit now
requires that prefix marker.

A completed production branch run at `a74ed2d1` on 2026-06-10 used the same
pinned WordPress ref and production build guards with
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=0`. Its WordPress job completed
successfully with the dedicated DB shard at `11.855s` shell real, the deferred
process-isolated shard at `103.426s`, the eager process-isolated shard at
`94.164s`, and the long non-isolated shard at `1156.633s`. The combined
test-only PHPUnit time was about `22.8` minutes, below main's same-ref
`28:21.227` all-in PHPUnit body. Normal CI therefore keeps JUnit logging off
for timing parity; the harness-owned slowest-class report remains opt-in with
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`.

## Optimization Assessment

The fresh ordinary WordPress and C API numbers remain in the documented trunk
parity band for the ordinary embedded path. The branch-specific cost that still
stands out is ownerless autocommit insert throughput. Its measured time is in
ownerless page-version publication, page-read refresh, and page-write refresh
paths that do not run for ordinary WordPress mysqli connections.

The next ownerless performance optimization should be bounded around redundant
page-write refresh or publish work, with correctness checks for active-reader
visibility and crash recovery. That belongs in an ownerless native-storage
slice, not in the WordPress PHPUnit CI split.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the WordPress `prepare-db` phase.
- Run a one-test `^Tests_DB` PHPUnit filter.
- Run a one-test negative-lookahead non-DB PHPUnit filter.
- Run a negative-lookahead filter aimed at a DB test and confirm no test runs.
- Run the split `^Tests_DB` WordPress PHPUnit suite.
- Run the `phpunit` phase against a prepared tree and confirm it validates the
  required build/setup artifacts before running PHPUnit.
- Run the WordPress mysqli `perf-probe` phase.
- Run the embedded C API performance probe with ownerless page-publish stats.
- Run `ctest --preset php-embedded-dev -L php --output-on-failure`.
- Run `cmake --build --preset format-check`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-08:

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- WordPress `prepare-db` passed and reported `wordpress_prepare_db_seconds=1`.
- `--filter '^Tests_DB::test_bail$'` passed one `Tests_DB` test.
- `--filter '^(?!Tests_DB).*Tests_Actions::test_simple_action$'` passed one
  non-DB test.
- `--filter '^(?!Tests_DB).*Tests_DB::test_bail$'` reported no tests executed,
  confirming the remaining-suite filter excludes DB tests.
- `--filter '^Tests_DB'` passed 651 tests with 3 skips and reported PHPUnit
  `Time: 00:17.172`, `wordpress_phpunit_shell_real_seconds=27.960`, and
  `wordpress_phpunit_seconds=28`.
- `MYLITE_WORDPRESS_PHASE=perf-probe` passed and reported the WordPress mysqli
  metrics recorded above.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and reported the C API metrics recorded above.
- The fresh `/tmp/mylite-mariadb-reference` main comparison passed with PHPUnit
  `Time: 00:23.168` and `wordpress_phpunit_seconds=39`.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed 3 tests.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

Follow-up local verification on 2026-06-08 for the test-only phase boundary:

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- An intentionally empty `MYLITE_WORDPRESS_PHASE=phpunit` run failed before
  PHPUnit with `Missing executable PHP wrapper` and instructed the caller to
  run `MYLITE_WORDPRESS_PHASE=build-php` first, confirming the phase does not
  fall back to setup or build work.
- `MYLITE_WORDPRESS_PHASE=perf-probe` with CI-sized iteration counts passed on
  the local default WordPress ref (`trunk`) and reported stock PHP process
  startup `580.218ms`, PHP wrapper startup with MyLite extensions `165.545ms`,
  process plus connect/close `571.828ms`, in-process mysqli connect/close
  `422.445ms`, `SELECT 1` `286.22 ops/s`, transactional inserts
  `326.53 ops/s`, point selects `233.84 ops/s`, prepared autocommit inserts
  `387.36 ops/s`, and direct-string autocommit inserts `764.33 ops/s`.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed 3 tests.

Follow-up verification on 2026-06-10 for the DB-prefix partition:

- A local PCRE sample rejected `Tests_DB::test_bail`,
  `Tests_DB_Charset::test_charset`, `Tests_DB_dbDelta::test_delta`, and
  `Tests_DB_RealEscape::test_real_escape`, while still matching
  `Tests_Actions::test_simple_action`.
- `tools/check-ci-production-builds` passed after adding the
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER: '^(?!Tests_DB)` audit marker.

## Acceptance Criteria

- CI shows separate visible step timings for the WordPress PHPUnit DB suite and
  the remaining WordPress PHPUnit suite.
- The two filters partition the `Tests_DB` class family without duplicating it
  in the remaining-suite step.
- Build, dependency, database preparation, and perf-probe steps remain separate
  from PHPUnit execution.
- The `phpunit` phase is test-only: missing build/setup/database artifacts
  produce an explicit phase-boundary error instead of silently doing setup work
  or failing later in PHPUnit.
- The mysqli `perf-probe` phase also requires the prepared database before it
  starts timing.
- The ordinary WordPress mysqli performance probe remains close to the pinned
  main baseline band.
- WordPress CI process/connect performance samples use five iterations for
  lower-noise startup timing while keeping the probe bounded.
- Remaining ownerless-only performance cost is documented separately from the
  ordinary PHPUnit path.
