# WordPress PHPUnit CI Phase Split

## Problem

The WordPress mysqli compatibility job still presents one large CI step for the
full wrapper invocation. That makes cold Docker/image state, WordPress checkout,
MariaDB embedded/PHP-extension builds, Composer dependency work, MyLite database
preparation, and the PHPUnit test body look like one timing bucket. The branch
has repeatedly shown focused `Tests_DB` runtime close to main, but slow full CI
samples are hard to triage because GitHub Actions does not show which harness
phase consumed the wall time.

The local developer command should remain a one-shot harness, because that path
is useful for reproducing WordPress compatibility failures without remembering a
phase sequence.

## Source Findings

- `.github/workflows/ci.yml` has one WordPress execution step named `Run
  WordPress PHPUnit suite`, which invokes
  `tools/wordpress-phpunit-mysqli-mylite`.
- `tools/wordpress-phpunit-mysqli-mylite` builds the Docker image on the host
  and then runs one container script that fetches WordPress, builds the MyLite
  embedded/PHP extension artifacts, installs Composer/PHPUnit dependencies,
  prepares a fresh MyLite database directory, and invokes PHPUnit.
- The wrapper already emits subphase timings such as
  `mylite_mariadb_embedded_seconds`, `mylite_php_build_seconds`,
  `wordpress_dependency_seconds`, `wordpress_prepare_db_seconds`, and
  `wordpress_phpunit_seconds`, but CI step timing still groups all of them under
  one visible step.
- The WordPress PHP extension path uses ordinary read/write embedded opens, not
  `MYLITE_OPEN_OWNERLESS_RW`, so this job is a regression sentinel for the
  non-ownerless mysqli runtime as well as broad application compatibility.

## Design

Add an explicit `MYLITE_WORDPRESS_PHASE` environment variable to the WordPress
harness:

- `all`: preserve the existing one-shot local behavior and run every phase.
- `docker-image`: build the Docker image and stop before starting a container.
- `setup`: run the container setup phases that fetch WordPress, build MyLite
  embedded/PHP artifacts, and install/patch PHPUnit dependencies.
- `prepare-db`: recreate the MyLite database directory and write
  `wp-tests-config.php` using the prepared PHP wrapper.
- `phpunit`: run only the PHPUnit command against the prepared WordPress tree
  and database.
- `perf-probe`: run an opt-in local measurement against the prepared PHP wrapper
  and database, covering PHP extension process startup, process startup plus
  MyLite connect/close, and in-process mysqli SQL loops.

Add `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1` for follow-on CI steps that reuse the
image built by the first phase. Invalid phase names or skip values fail before
doing work.

Split the CI job into separate steps that invoke those phases in order:

1. Build the Docker image.
2. Build the WordPress PHPUnit environment.
3. Prepare the WordPress PHPUnit database.
4. Run the WordPress PHPUnit suite.

This gives GitHub step-level timings without changing the pinned WordPress ref,
the database placement, the build targets, the JUnit artifact behavior, or the
default local command.

## Compatibility Impact

No SQL, mysqli, PHP API, or MyLite runtime behavior changes. The same WordPress
PHPUnit suite runs with the same PHP wrapper and extension modules. The split is
observability and harness orchestration only.

## Directory And Lifecycle Impact

No new durable files are introduced. The database directory still defaults to a
host temporary path and `prepare-db` continues to recreate it before a suite
run. The prepared WordPress checkout, Composer cache, MariaDB embedded build,
and PHP extension build remain under the existing build directories.

## Build And Performance Impact

The split makes cold Docker image construction, MariaDB/PHP build work,
dependency setup, database creation, and PHPUnit execution separately visible in
CI. It does not itself reduce MyLite startup cost or engine execution time.
That visibility is required before attributing a slow job to ordinary mysqli
runtime, per-process embedded startup, or setup/cache variance.

The `perf-probe` phase gives a smaller local signal for the same question:
per-process startup/connect cost is separated from steady in-process `SELECT 1`,
transactional insert, and primary-key point-select loops. The probe is not a
drop-in benchmark for WordPress, but it shows whether a slow PHPUnit run is
likely coming from PHP process churn or from the embedded SQL engine after a
connection is already open.

On 2026-06-07, the first split local run on the pinned CI WordPress ref showed
the intended step boundaries: cached Docker image build `3s`, warmed setup
`17s`, database preparation `2s` inside the container, and focused `Tests_DB`
PHPUnit `00:24.932` with `wordpress_phpunit_seconds=43`. After removing
ordinary-path prepared-statement SQL text retention, the same focused PHPUnit
phase reported `00:22.970`, `wordpress_phpunit_shell_real_seconds=39.221`, and
`wordpress_phpunit_seconds=39`.

The same probe separated process startup from engine execution. Against a fresh
host-`/tmp` database on this branch, the warmed post-optimization run reported
PHP extension process startup `74.811ms`, process plus MyLite connect/close
`542.647ms`, `SELECT 1` `257.03 ops/s`, transactional inserts `405.42 ops/s`,
and primary-key point selects `247.00 ops/s`. A same-machine main
`4760d512` probe reported process startup `94.602ms`, process plus connect/close
`515.826ms`, `SELECT 1` `262.22 ops/s`, transactional inserts `387.10 ops/s`,
and point selects `252.97 ops/s`. That keeps startup and steady engine loops
close to trunk; the remaining differences are small relative to local runner and
database-state variance.

After the later ownerless slices through `d58e12b2`, a current audit found that
the host-side `MYLITE_WORDPRESS_PERF_*` iteration overrides were not forwarded
into the Docker container. The harness now forwards
`MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS`,
`MYLITE_WORDPRESS_PERF_SQL_ITERATIONS`, and
`MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS`, preserving the existing defaults when
unset. A patched `/tmp` ownerless worktree confirmed the override with
`10` process iterations, `10000` read iterations, and `2000` write iterations.
That run reported PHP process startup `94.331ms`, process plus MyLite
connect/close `555.605ms`, `SELECT 1` `196.21 ops/s`, transactional inserts
`313.43 ops/s`, and primary-key point selects `195.97 ops/s` under the current
host load. An inline same-machine main `4760d512` mysqli probe reported process
startup `162.715ms`, process plus connect/close `642.132ms`, `SELECT 1`
`244.61 ops/s`, transactional inserts `389.27 ops/s`, and point selects
`237.88 ops/s`. A lower-level `mylite` PHP extension read probe kept the core
engine path close to main: branch query-per-call `SELECT 1` `254.98 ops/s` and
prepared-reuse `SELECT 1` `378.55 ops/s`, versus main `250.91 ops/s` and
`387.91 ops/s`. The current simple mysqli loop therefore points at wrapper or
metadata microbenchmark overhead rather than a core engine startup regression.

`phpunit` phase output remains the primary application-runtime signal:
PHPUnit's own `Time:` line plus `wordpress_phpunit_shell_real_seconds`,
`wordpress_phpunit_shell_user_seconds`, `wordpress_phpunit_shell_sys_seconds`,
and `wordpress_phpunit_seconds`.

The CI perf-probe summary now repeats the requested CMake build type and the
process, connect, SQL, and write iteration counts. It also emits explicit
process-connect summary aliases so branch/main log comparisons do not confuse a
full PHP process plus connect/close sample with the active-runtime in-process
reconnect sample.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `MYLITE_WORDPRESS_PHASE=docker-image tools/wordpress-phpunit-mysqli-mylite`
  and confirm it builds or reuses the Docker image without starting the
  container.
- Run the pinned `Tests_DB` path through `setup`, `prepare-db`, and `phpunit`
  phases with `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1`.
- Run the perf probe after setup/database preparation and confirm it reports
  process and SQL loop timings:
  `MYLITE_WORDPRESS_PHASE=perf-probe MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 tools/wordpress-phpunit-mysqli-mylite`.
- Run the perf probe with non-default `MYLITE_WORDPRESS_PERF_*` values and
  confirm the selected iteration counts are visible in the container output and
  in the `wordpress_perf_summary_*` lines.
- Confirm the default `all` phase remains valid.
- Run `git diff --check`.

## Acceptance Criteria

- The WordPress harness validates and reports the selected phase.
- The default local wrapper invocation still runs the full suite path.
- CI shows separate WordPress steps for Docker image build, environment build,
  database preparation, and PHPUnit execution.
- The `phpunit` phase can run without repeating setup or database preparation.
- The split preserves the existing JUnit artifact upload behavior.
- The opt-in `perf-probe` phase reports per-process startup/connect cost and
  in-process SQL loop throughput without changing the default local or CI suite.
