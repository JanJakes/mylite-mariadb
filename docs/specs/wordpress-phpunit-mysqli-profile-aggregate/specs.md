# WordPress PHPUnit MySQLi Profile Aggregate

## Problem Statement

`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` is the opt-in path for
understanding where WordPress PHPUnit spends MyLite mysqli adapter time. The
current timing summary records the last selected `mylite_mysqli_profile_*`
value from the captured PHPUnit output. That works for a single long
non-isolated PHP process, but process-isolated PHPUnit runs can emit one
profile block from the parent and one block from each child process.

Using the last value alone can hide child-side MyLite open/close cost, so it
is not enough evidence for the current performance question: whether PHPUnit
slowdown comes from per-process startup, engine execution, or parent reconnect
policy.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` prints one
  `mylite_mysqli_profile_*` block at PHP module shutdown when
  `MYLITE_MYSQLI_PROFILE=1` and profiled work happened. Each block includes
  `mylite_mysqli_profile_enabled=1`, `mylite_mysqli_profile_pid`,
  open/close counters and milliseconds, query counters and milliseconds, and
  fetch counters.
- `tools/wordpress-phpunit-mysqli-mylite` maps
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` to `MYLITE_MYSQLI_PROFILE=1`
  for the PHPUnit PHP process. PHPUnit child processes inherit that
  environment.
- The same harness captures PHPUnit output in a temporary file and currently
  appends selected mysqli profile keys to the timing summary by taking the last
  value for each key.
- Printing `mylite_mysqli_profile_*` rows from a process-isolated child to
  stderr can make PHPUnit treat the child as failed output, so child-side
  profiles need a harness-owned file sink rather than raw child stderr.
- Normal CI keeps `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=0`, so this
  aggregation remains opt-in diagnostic behavior.

## Design

Add an optional `MYLITE_MYSQLI_PROFILE_OUTPUT` sink to the PHP extension.
When the variable is set, each profiled PHP process appends its profile block
to that file under an exclusive file lock. When it is not set, direct
extension usage keeps the existing stderr output.

The WordPress harness sets `MYLITE_MYSQLI_PROFILE_OUTPUT` only for
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`. After PHPUnit exits, the harness
appends the file contents to the captured output and removes the temporary
file. This keeps raw profile rows visible in logs without exposing them to
PHPUnit's child-process output checks.

Keep the existing last-value summary rows for backward compatibility. Add new
`mylite_mysqli_profile_aggregate_*` rows when one or more mysqli profile blocks
are present in the combined captured output.

The aggregate parser sums additive counters and millisecond totals across all
profile blocks in the phase output, then derives aggregate averages:

- `mylite_mysqli_profile_aggregate_processes`;
- open and close calls, close-kind calls, total milliseconds, and average
  milliseconds;
- query call mix, query milliseconds, query average milliseconds, cache,
  prepare, status-sync, result/no-result, direct libmylite text-execution
  subphase, row, and fetch-object totals.

Do not aggregate per-process average rows directly. Compute aggregate averages
from summed totals divided by summed call counts.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli API, native storage, or WordPress test
behavior changes. The slice only adds opt-in diagnostic rows to the timing
summary after a PHPUnit phase completes.

## Directory And Lifecycle Impact

No durable directory-layout changes. The harness already captures PHPUnit
output in a temporary file and removes it after post-processing. This slice
adds a second temporary profile sink only while mysqli profiling is enabled,
appends that sink into the captured PHPUnit output after PHPUnit exits, and
then removes it.

## Native Storage Impact

No native storage format, recovery, or locking changes.

## Build, Size, License, And Dependencies

No dependency or license impact. The PHP extension adds only an opt-in profile
output path and file lock around diagnostic output. The added shell and `awk`
post-processing runs only after profiled PHPUnit phases.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a focused production process-isolated WordPress PHPUnit test with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`.
- Verify the custom timing summary includes the existing last-value
  `mylite_mysqli_profile_*` rows and the new
  `mylite_mysqli_profile_aggregate_*` rows.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `git diff --check`.

## Acceptance Criteria

- Unprofiled PHPUnit phases keep the existing timing summary shape.
- Profiled PHPUnit phases preserve the existing last-value profile rows.
- Profiled process-isolated PHPUnit phases append aggregate profile rows that
  count more than one profile process when parent and child profiles are
  emitted.
- Aggregate open/close and query averages are derived from summed totals and
  counts, not by averaging per-process averages.
- Production CI guard checks continue to pass.

## Verification Results

Completed.

## Evidence

The PHP extension rebuilt under the production PHP embedded preset:

```text
cmake --build --preset php-embedded-prod --target mylite_mysqli_php_extension -j2
ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.profile$' --output-on-failure
```

The WordPress production build cache was refreshed with Release first-party
targets and the guarded MariaDB embedded archive:

```text
MYLITE_WORDPRESS_PHASE=build-php \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-profile-aggregate-build \
tools/wordpress-phpunit-mysqli-mylite
```

A focused process-isolated production PHPUnit run enabled both child-process
profiling and mysqli profiling:

```text
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-profile-aggregate \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-profile-aggregate/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end$'
```

The run passed one PHPUnit test and kept child profile rows out of PHPUnit's
stderr stream. The timing summary retained existing last-value rows such as
`mylite_mysqli_profile_open_calls=3` and
`mylite_mysqli_profile_close_calls=3`, and added aggregate rows:

```text
mylite_mysqli_profile_aggregate_processes=4
mylite_mysqli_profile_aggregate_open_calls=7
mylite_mysqli_profile_aggregate_open_ms_total=1351.467
mylite_mysqli_profile_aggregate_open_ms_avg=193.067
mylite_mysqli_profile_aggregate_close_calls=7
mylite_mysqli_profile_aggregate_close_explicit_calls=1
mylite_mysqli_profile_aggregate_close_object_free_calls=6
mylite_mysqli_profile_aggregate_close_ms_total=2488.633
mylite_mysqli_profile_aggregate_close_ms_avg=355.519
mylite_mysqli_profile_aggregate_query_calls=953
mylite_mysqli_profile_aggregate_query_ms_total=1915.763
mylite_mysqli_profile_aggregate_query_ms_avg=2.010
mylite_mysqli_profile_aggregate_fetch_object_calls=1807
mylite_mysqli_profile_aggregate_fetch_object_ms_total=1.223
mylite_mysqli_profile_aggregate_fetch_object_ms_avg=0.001
```

The same summary reported
`wordpress_phpunit_shell_real_seconds=18.476`,
`wordpress_phpunit_reported_seconds=6.081`, one profiled child process,
`wordpress_phpunit_child_process_runtime_ms_avg=4026.967`, and
`wordpress_phpunit_child_process_reconnect_ms_avg=107.983`.

The syntax, production guard, and whitespace checks passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
cmake --build --preset format-check-prod
git diff --check
```

## Risks And Follow-Up

- The aggregate rows are diagnostics only. They do not reduce child process
  runtime.
- Aggregation covers the selected high-value profile keys, not every key
  printed by the PHP extension. Additional keys can be added when a future
  investigation needs them.
