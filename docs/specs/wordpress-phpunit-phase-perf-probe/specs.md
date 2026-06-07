# WordPress PHPUnit Phase Performance Probe

## Problem

The WordPress PHPUnit job already separated the full PHPUnit command from the
coarse environment setup phase, but setup still combined source fetch, native
embedded/PHP extension build work, and Composer/PHPUnit dependency installation
into one CI step. That makes it harder to distinguish a slow PHP test body from
a cold MariaDB embedded rebuild, dependency cache miss, or WordPress checkout
refresh.

The existing opt-in WordPress `perf-probe` phase measured PHP process startup,
process plus MyLite connect/close, and steady mysqli SQL loops, but CI did not
run it. The probe also did not separate stock PHP process startup from startup
with the MyLite extensions loaded, or process churn from in-process MyLite
connect/close cost. It also measured only transaction-amortized prepared
inserts, leaving WordPress-style autocommit write cost visible only in the
lower-level embedded C API probe.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` runs the `wordpress-phpunit` job through
  `tools/wordpress-phpunit-mysqli-mylite` with explicit harness phases.
- `tools/wordpress-phpunit-mysqli-mylite` supports separate Docker image,
  setup, database preparation, PHPUnit, and `perf-probe` phases. The prior
  `setup` phase performs WordPress fetch, MariaDB embedded ensure, MyLite PHP
  extension configure/build, PHP wrapper validation, WordPress Composer
  install, PHPUnit tool install, and the PHPUnit child-process patch.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens WordPress
  mysqli connections with `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE`, not
  `MYLITE_OPEN_OWNERLESS_RW`. The WordPress job therefore remains an ordinary
  embedded mysqli workload, not an ownerless-mode benchmark.
- `docs/specs/wordpress-phpunit-embedded-build-fast-path/specs.md` already
  records that focused `Tests_DB` timing is close to pinned main when source
  and database storage placement are comparable, and that slow-looking totals
  are often build/setup, filesystem placement, or GitHub runner-band effects.
- `docs/specs/ordinary-ownerless-startup-performance-probe/specs.md` covers the
  lower-level C API probe that CI runs as a separate embedded performance step.

## Design

Add narrower WordPress harness phases while preserving the existing local
default:

- `setup` remains a compatibility alias that runs fetch, build, and dependency
  installation in sequence.
- `fetch` only refreshes/checks out the pinned WordPress source tree.
- `build-php` only ensures the MariaDB embedded archive, configures/builds the
  MyLite PHP extension targets, and validates the wrapper.
- `dependencies` only installs WordPress Composer dependencies, the PHPUnit
  tool dependency, and the MyLite child-process lock-release patch.
- `prepare-db`, `perf-probe`, and `phpunit` keep their existing responsibilities.

Update CI to call these sub-phases as separate steps. The PHPUnit suite remains
its own final test step, so GitHub Actions step timing now separates native
builds and dependency setup from the PHP test body. CI does not enable
PHPUnit's optional JUnit logger by default; the isolated step duration and the
`wordpress_phpunit_shell_*`/`wordpress_phpunit_seconds` log keys provide the
suite timing without adding per-test report generation work that main did not
perform.

Run the WordPress mysqli `perf-probe` as a separate CI step after database
preparation. Keep CI iteration counts smaller than the local defaults so the
step is useful but not a large new source of wall time.

Deepen the `perf-probe` output:

- stock `/usr/local/bin/php -r ''` process startup,
- PHP wrapper startup with MyLite extensions loaded,
- PHP process startup plus MyLite mysqli connect/close,
- derived process-plus-connect delta,
- repeated in-process mysqli connect/close through the same wrapper,
- steady in-process direct `SELECT 1`,
- transactional prepared inserts,
- autocommit prepared inserts,
- autocommit direct `mysqli_query()` inserts,
- primary-key point selects.

## Compatibility Impact

No SQL, PHP API, mysqli, public C API, storage-engine, or runtime behavior
changes. The slice only changes the integration harness, CI step boundaries,
and diagnostic output.

## Directory And Lifecycle Impact

No durable directory-layout changes. The performance probe reuses the prepared
WordPress MyLite database directory and drops its own temporary probe table
before exiting.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage format changes. The probe uses the ordinary mysqli adapter
and an InnoDB table already created inside the MyLite-owned WordPress test
directory.

## Build And Performance Impact

CI logs now expose WordPress fetch, PHP extension build, dependency install,
database preparation, performance probe, and PHPUnit suite wall time as
separate GitHub Actions steps. The added CI `perf-probe` uses three process and
connect iterations, 1000 SQL iterations, and 200 insert iterations to keep the
step bounded. The autocommit insert loop reuses that same insert iteration
control for prepared and direct-string insert timings. CI leaves
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT` at its harness default of `0` so suite
timing remains comparable with main's non-JUnit WordPress run.

Local default behavior is preserved: running `tools/wordpress-phpunit-mysqli-mylite`
without `MYLITE_WORDPRESS_PHASE` still executes the full end-to-end harness.
The old `setup` phase remains supported for callers that do not need the finer
CI split.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `MYLITE_WORDPRESS_PHASE=fetch MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  tools/wordpress-phpunit-mysqli-mylite` on an already-built Docker image, when
  local Docker is available.
- Run `MYLITE_WORDPRESS_PHASE=build-php MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  tools/wordpress-phpunit-mysqli-mylite` on a warmed tree, when local Docker is
  available.
- Run `MYLITE_WORDPRESS_PHASE=dependencies MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  tools/wordpress-phpunit-mysqli-mylite` after fetch, when local Docker is
  available.
- Run `MYLITE_WORDPRESS_PHASE=prepare-db MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  tools/wordpress-phpunit-mysqli-mylite` after build, when local Docker is
  available.
- Run `MYLITE_WORDPRESS_PHASE=perf-probe MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=1
  MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS=1
  MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=5
  MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=2
  tools/wordpress-phpunit-mysqli-mylite` after database preparation, when local
  Docker is available.
- Run the focused PHP CTest labels in `php-embedded-dev`.
- Run the embedded C API performance probe.
- Run `cmake --build --preset format-check`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-07 used the pinned CI WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, an existing Docker image, warmed
build trees, and the default host-temp WordPress database placement.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `fetch` restored the pinned WordPress checkout and reported
  `wordpress_fetch_seconds=1`.
- `build-php` reported `mariadb_embedded_configure=skipped`,
  `mylite_build_seconds=16`, and `wordpress_total_seconds=18` after rebuilding
  changed local artifacts.
- `dependencies` reported `wordpress_dependency_seconds=7` and
  `wordpress_total_seconds=8`.
- `prepare-db` reported `wordpress_prepare_db_seconds=1` and
  `wordpress_total_seconds=2`.
- A reduced `perf-probe` with one process/connect iteration, five SQL
  iterations, and two insert iterations passed and printed the new metric keys;
  a later reduced rerun after adding autocommit insert reporting printed
  `wordpress_perf_insert_autocommit_iterations=2` and
  `wordpress_perf_insert_autocommit_ops_per_second=316.08`.
- The CI-sized local `perf-probe` reported stock PHP startup `51.733ms`,
  PHP-with-MyLite-extension startup `74.387ms`, process plus connect/close
  `557.133ms`, derived process/connect delta `482.746ms`, in-process
  connect/close `382.172ms`, `SELECT 1` `258.59 ops/s`, transactional inserts
  `371.79 ops/s`, point selects `240.40 ops/s`, and
  `wordpress_total_seconds=14`.
- The isolated `phpunit --filter Tests_DB` phase reported PHPUnit `00:21.711`,
  `wordpress_phpunit_shell_real_seconds=40.324`, and
  `wordpress_phpunit_seconds=40` for 651 tests with 3 skips.
- A later CI-sized local branch/main comparison using the same Docker image and
  `/tmp`-mounted database placement did not show an ordinary mysqli engine
  regression on the branch: branch `SELECT 1` was `266.64 ops/s`, point select
  `252.50 ops/s`, and autocommit insert `343.01 ops/s`; same-host main
  reported `233.84 ops/s`, `223.95 ops/s`, and `337.02 ops/s`, respectively.
  That evidence points at harness/reporting and full-suite shape before the
  ordinary mysqli engine path.
- The backward-compatible `setup` alias still ran fetch, build, and dependency
  phases in one invocation and completed with `wordpress_total_seconds=13` on
  the warmed tree.
- `build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed with ordinary warm open/close `376.004ms`, ownerless warm open/close
  `400.504ms`, ordinary direct `SELECT 1` `4101.29 ops/s`, ownerless direct
  `SELECT 1` `3243.86 ops/s`, ordinary transactional inserts `2140.05 ops/s`,
  and ownerless transactional inserts `1050.36 ops/s`.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed after
  rebuilding the stale local `mylite_pdo_php_extension` target.
- `cmake --build --preset format-check` and `git diff --check` passed.

## Acceptance Criteria

- CI has separate visible steps for WordPress source fetch, MyLite PHP extension
  build, WordPress/PHPUnit dependency installation, database preparation,
  WordPress mysqli performance probing, and the PHPUnit suite.
- The `phpunit` phase remains separated from builds and dependency setup.
- CI's default WordPress PHPUnit suite run does not enable optional JUnit
  logging; callers can still opt in with `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`
  when they need an XML report.
- Existing `all` and `setup` phase behavior remains available for local
  callers.
- The WordPress `perf-probe` prints parseable process startup, extension-load
  startup, process-plus-connect, in-process connect/close, and steady SQL
  throughput keys, including separate transactional and autocommit insert
  rates for prepared statements and direct `mysqli_query()` strings.
- Focused verification passes without changing SQL behavior.

## Risks And Follow-Up

- Timing is still host-sensitive. Treat the new metrics as branch/main trend
  evidence and as a way to classify slow jobs, not as universal benchmark
  thresholds.
- Local ownerless C API profiling on 2026-06-07 showed ownerless autocommit
  inserts remain much slower than ordinary autocommit under FULL, NORMAL, and
  OFF durability because the ownerless native publication path pays page-log,
  checkpoint, and dirty-page visibility costs. That is a separate ownerless
  optimization target; the WordPress PHPUnit job opens the ordinary mysqli
  path.
- Future CI policy can add soft thresholds after enough samples exist, but this
  slice keeps the new timing step non-fatal except for functional probe errors.
