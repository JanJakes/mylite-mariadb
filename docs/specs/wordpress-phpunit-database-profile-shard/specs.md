# WordPress PHPUnit Database Profile Shard

## Problem

The WordPress CI job now separates production build, database preparation,
performance probe, process-isolated PHPUnit, and non-isolated PHPUnit timing.
Those normal test-only shards deliberately keep `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=0`
so their wall-clock timings remain comparable to production-shaped branch and
trunk runs.

That leaves a performance attribution gap in default CI. The `phpunit-db` shard
is small enough to run as a repeatable diagnostic, and its query mix exercises
the hot WordPress transaction, DDL, `SET`, result-query, and direct-execution
paths that have driven recent mysqli optimization work. Without a profiled
diagnostic phase, each investigation has to reproduce a local profiled run on a
host whose load and database placement may not match CI.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` already maps
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` to `MYLITE_MYSQLI_PROFILE=1`
  for a PHPUnit phase, captures `mylite_mysqli_profile_*` output, and appends
  selected last-process and aggregate profile rows to
  `MYLITE_WORDPRESS_TIMING_SUMMARY_PATH`.
- `.github/workflows/ci.yml` keeps the normal `phpunit-db`,
  process-isolated, and non-isolated test-only shards unprofiled. That protects
  their wall-clock timing but means the uploaded WordPress timing artifact has
  no default adapter/query attribution rows.
- A current focused production `^Tests_DB` profile on this branch, using the
  guarded `build/wordpress-php-embedded-prod` Release build and
  `build/wordpress-mariadb-embedded` MinSizeRel archive, passed 651 tests with
  3 skips. Because the local keepalive diagnostic hit the known second-handle
  open failure, the accepted local sample used the same `^Tests_DB` filter
  without keepalive. It reported `query_ms_total=5290.576`,
  `query_verb_select_ms_total=2589.649`,
  `query_verb_transaction_ms_total=1584.162`,
  `query_verb_ddl_ms_total=573.732`,
  `query_verb_set_ms_total=157.835`,
  `libmylite_exec_result_mysql_query_ms_total=3624.755`, and
  `libmylite_exec_result_native_control_ms_total=1572.866`.
- The existing `docs/specs/libmylite-transaction-end-profile/specs.md` records
  that an embedded helper prototype for exact default `COMMIT` and `ROLLBACK`
  regressed the focused production `^Tests_DB` profile and the supporting
  micro-benchmark. Transaction-end parser bypass is therefore not a safe next
  optimization without new evidence.

## Design

Add one explicit CI diagnostic phase after the normal unprofiled database shard:

- keep `Run WordPress PHPUnit database suite (test only)` unchanged with timing
  label `phpunit-db` and profiling disabled;
- add `Run WordPress PHPUnit database profile probe (diagnostic)` with timing
  label `phpunit-db-profile`;
- run the same `--filter '^Tests_DB'` against the prepared production database
  baseline with `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`;
- enable only `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`; keep JUnit logging,
  child profiling, and child timing disabled through the existing CI defaults;
- keep the same `MinSizeRel` MariaDB archive and `Release` MyLite PHP build
  guards in the step body before publishing timing/profile rows.

Extend `tools/check-ci-production-builds` so CI fails if the diagnostic step is
removed, loses its production build guards, stops using the `^Tests_DB` filter,
or if the normal unprofiled `phpunit-db` shard accidentally enables mysqli
profiling.

## Compatibility Impact

No SQL behavior, mysqli API behavior, public C API behavior, storage format,
ownerless concurrency behavior, or WordPress test selection changes. The normal
test coverage still runs in the existing shards; the new phase is diagnostic
and repeats the database shard for profile attribution.

## Directory And Lifecycle Impact

No durable directory-layout changes. Like every `phpunit` phase with
`MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, the harness restores the prepared
WordPress MyLite baseline before the diagnostic run and writes only transient
PHPUnit/profile output plus the existing timing summary under the build report
directory.

## Build, Size, License, And Dependency Impact

No compiled code, dependency, license, or binary-size changes. CI wall time
increases by one bounded `^Tests_DB` run, but the main production timing shards
remain unprofiled and comparable.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run the local focused production diagnostic command when the WordPress build
  cache is available:
  `MYLITE_WORDPRESS_PHASE=phpunit`, `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1`,
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`,
  `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, and `--filter '^Tests_DB'`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- CI keeps an unprofiled `phpunit-db` production timing step.
- CI also publishes a separate `phpunit-db-profile` diagnostic step with
  selected `mylite_mysqli_profile_*` and aggregate profile rows in the
  WordPress timing summary.
- The diagnostic step uses production build guards and the same `^Tests_DB`
  filter as the normal database shard.
- Production-build audit fails if the diagnostic step drifts or if the normal
  database timing shard enables mysqli profiling.

## Verification Results

Local verification completed on the active `ownerless-concurrency` worktree:

```text
bash -n tools/check-ci-production-builds
bash -n tools/wordpress-phpunit-mysqli-mylite
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
```

The direct audit reported
`ci_production_build_audit_ok=.github/workflows/ci.yml`, and the production
CTest wrapper passed one `tools.ci-production-builds` test.

The focused production diagnostic command also passed:

```text
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 \
MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1 \
MYLITE_WORDPRESS_TIMING_LABEL=ci-db-profile-diagnostic \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/ci-db-profile-diagnostic/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB'
```

It ran 651 tests with 3 skips and reported PHPUnit `8.951s`, shell real
`17.630s`, and total harness time `24s`. The timing summary included both
per-process and aggregate profile rows, including:

```text
mylite_mysqli_profile_query_calls=3914
mylite_mysqli_profile_query_ms_total=5529.911
mylite_mysqli_profile_query_verb_select_ms_total=2684.081
mylite_mysqli_profile_query_verb_transaction_ms_total=1661.307
mylite_mysqli_profile_query_transaction_start_ms_total=832.860
mylite_mysqli_profile_query_transaction_end_ms_total=828.448
mylite_mysqli_profile_libmylite_exec_result_mysql_query_ms_total=3783.149
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=1650.158
mylite_mysqli_profile_aggregate_processes=1
mylite_mysqli_profile_aggregate_query_ms_total=5529.911
mylite_mysqli_profile_aggregate_libmylite_exec_result_native_control_rollback_calls=651
```

## Risks And Follow-Up

- The diagnostic repeat adds CI wall time. It is intentionally scoped to the
  small database shard rather than the long non-isolated shards, because the
  latter are the primary production timing signal.
- The profile rows identify optimization targets but are not themselves a
  runtime speedup. Current evidence still points at MariaDB text execution,
  transaction-control wrappers, and SELECT-heavy WordPress query work; the
  known transaction-end helper remains rejected until a future design beats the
  recorded controls.
