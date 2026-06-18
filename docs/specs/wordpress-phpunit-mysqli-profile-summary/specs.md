# WordPress PHPUnit MySQLi Profile Summary

## Problem Statement

The WordPress PHPUnit CI job now splits build, setup, performance probe, and
test-only phases, and the non-isolated shard enables the opt-in
`MYLITE_MYSQLI_PROFILE=1` adapter profile. The current completed production CI
run shows the branch PHPUnit test-only phases are not slower than the latest
main one-shot run, but the remaining non-isolated cost is still dominated by
mysqli query execution.

Those `mylite_mysqli_profile_*` buckets are present only in the raw job log.
The timing summary table published by CI contains phase wall times and compact
WordPress perf-probe keys, but it does not expose the profile totals that show
whether the next optimization should target open/close lifecycle, result
queries, no-result execution, cache behavior, or fetch/materialization work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` captures PHPUnit output in the
  `phpunit` phase, parses `wordpress_phpunit_*` timing keys, and appends those
  keys to `MYLITE_WORDPRESS_TIMING_SUMMARY_PATH`.
- The same harness enables the adapter profile for a PHPUnit phase only when
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, which maps to
  `MYLITE_MYSQLI_PROFILE=1` for the PHP process.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` prints
  `mylite_mysqli_profile_*` counters at PHP module shutdown when the profile is
  enabled and at least one profiled operation ran.
- `.github/workflows/ci.yml` enables mysqli profiling only for the slow
  non-isolated remaining WordPress PHPUnit shard and keeps the database and
  process-isolated shards unprofiled.
- The latest completed branch CI run before this slice reported split
  WordPress test-only shell timings of about `867` seconds total
  (`9.526`, `84.121`, `58.235`, and `715.139` seconds), while the latest main
  WordPress run reported a one-shot PHPUnit body time of `1701.227` seconds and
  `wordpress_phpunit_seconds=1706`.

## Design

Extend the existing PHPUnit output post-processing in
`tools/wordpress-phpunit-mysqli-mylite`:

- keep normal streamed PHPUnit output unchanged;
- after `wordpress_phpunit_*` timing extraction, scan the same captured output
  for selected `mylite_mysqli_profile_*` keys;
- for each selected key, append only the last value to the timing summary,
  because profile output can include an earlier WordPress install/bootstrap PHP
  process before the main PHPUnit process exits;
- append nothing when the profile is disabled or no profile output is present.

The selected keys summarize the major buckets needed for the next optimization
decision: open/close, query call mix, query total/average, cache hits/misses,
prepare/cache clear/status sync, result/no-result execution, result row count,
fetch-object conversion, and direct libmylite text-execution subphases when
the bundled libmylite profile counters are available.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, storage, or WordPress behavior
changes. This slice changes diagnostics emitted into the CI timing summary.

## Directory And Lifecycle Impact

No durable directory-layout changes. The harness already captures PHPUnit
output in a temporary file and removes it before exit; this slice reads the same
temporary file before removal.

## Native Storage Impact

No native storage format or ownerless storage behavior changes.

## Build, Size, License, And Dependencies

No compiled code, binary size, license, or dependency changes. The added shell
logic runs only after a PHPUnit phase finishes.

## Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a focused production WordPress PHPUnit phase with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` and a custom timing-summary path,
  then verify selected `mylite_mysqli_profile_*` rows appear in the summary.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Completed.

## Evidence

The syntax and production-build audit checks passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite
tools/check-ci-production-builds
ci_production_build_audit_ok=/home/agent/.paseo/worktrees/1irk9sr4/greasy-ostrich/.github/workflows/ci.yml
```

A focused production WordPress PHPUnit run used the existing guarded
`build/wordpress-php-embedded-prod` Release build, the
`build/wordpress-mariadb-embedded` MinSizeRel archive, and the prepared
external `/tmp` WordPress MyLite database. The run enabled
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` and wrote a custom timing summary:

```text
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-profile \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-profile-summary/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB::test_bail$'
```

The run passed one PHPUnit test, emitted an earlier install-process profile,
then emitted the main PHPUnit-process profile. The timing summary recorded the
last profile values, including `mylite_mysqli_profile_open_calls=2`,
`mylite_mysqli_profile_close_calls=2`,
`mylite_mysqli_profile_query_calls=129`,
`mylite_mysqli_profile_exec_result_ms_total=129.011`,
`mylite_mysqli_profile_exec_no_result_ms_total=100.314`,
`mylite_mysqli_profile_fetch_object_calls=336`, and
`mylite_mysqli_profile_fetch_object_ms_total=0.312`. The same summary kept the
existing PHPUnit rows, including `wordpress_phpunit_shell_real_seconds=22.332`
and `wordpress_phpunit_reported_seconds=1.654`.

The matching focused unprofiled run passed with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=0`; its timing summary contained only
the existing `wordpress_*` rows and no `mylite_mysqli_profile_*` rows:

```text
unprofiled_summary_profile_rows=0
```

The focused production CTest guard and formatting checks also passed:

```text
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
100% tests passed, 0 tests failed out of 1

cmake --build --preset format-check-prod
git diff --check
```

## Acceptance Criteria

- Unprofiled PHPUnit phases keep the existing timing summary shape.
- Profiled PHPUnit phases append selected `mylite_mysqli_profile_*` rows under
  the same phase label.
- When multiple profile summaries appear in one PHPUnit log, the timing summary
  records the last value for each selected key.
- CI production-build and timing guard checks continue to pass.

## Risks And Follow-Up

- The profile rows are diagnostics, not a runtime optimization. The follow-up
  `wordpress-phpunit-mysqli-profile-aggregate` slice adds aggregate rows for
  process-isolated runs, so child-process cost investigations should use the
  aggregate counters instead of relying only on the last profile block.
- The current next optimization target remains engine/query execution inside
  the non-isolated shard and ownerless/native page-publication cost, not PHP
  fetch-object conversion or repeated full open/close in the keepalive-enabled
  non-isolated shard.
