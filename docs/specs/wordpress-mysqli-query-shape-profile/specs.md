# WordPress Mysqli Query Shape Profile

## Problem

The production WordPress database profile shard now publishes mysqli adapter
verb buckets for the bounded `^Tests_DB` PHPUnit run. That showed SELECT and
transaction/native-control work are hot, but the SELECT bucket is still too
coarse to choose a safe next optimization. The rejected
`libmylite-transaction-end-profile` helper also showed that optimizing from a
coarse bucket can regress focused controls.

This slice adds SQL-shape attribution to the existing opt-in mysqli profile so
CI timing artifacts identify the exact query families consuming the profiled
database shard. It is diagnostic-only; it does not change SQL execution,
ownerless coordination, or storage behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  centralizes text-query profiling in `php_mylite_mysqli_query_impl()` and
  `php_mylite_mysqli_profile_finish_query()`. Existing profile output is
  guarded by `MYLITE_MYSQLI_PROFILE=1` and printed during module shutdown.
- `tools/wordpress-phpunit-mysqli-mylite` already copies selected
  `mylite_mysqli_profile_*` rows into `MYLITE_WORDPRESS_TIMING_SUMMARY_PATH`
  for profiled phases.
- The green CI run for `5c4aee8a` reported the new `phpunit-db-profile`
  diagnostic shard at about 10 seconds wall time, with `query_calls=4010`,
  `query_ms_total=2912.166`, `query_verb_select_ms_total=1241.496`,
  `query_verb_transaction_ms_total=892.218`,
  `query_verb_ddl_ms_total=445.893`, and
  `libmylite_exec_result_native_control_ms_total=891.803`.
- `docs/specs/libmylite-transaction-end-profile/specs.md` records that an
  exact default `COMMIT`/`ROLLBACK` helper regressed the focused production
  WordPress profile and parser-control micro-benchmark. Transaction-end parser
  bypass remains rejected without new evidence.

## Design

Extend the PHP mysqli adapter's opt-in profile with a bounded query-shape table:

- normalize each profiled text query by uppercasing ASCII, collapsing
  whitespace, replacing string and numeric literals with `?`, and bounding the
  retained sample text;
- hash the normalized shape with a process-local FNV-1a style 64-bit hash so
  equivalent literal variants group together without storing unbounded SQL;
- retain at most 128 tracked shapes and print the top five by total elapsed
  query time, with overflow call/time counters for replaced or untracked shapes;
- emit `mylite_mysqli_profile_query_shape_*` rows only when
  `MYLITE_MYSQLI_PROFILE=1` is already enabled;
- copy the top-shape rows into the WordPress timing summary for profiled
  phases, including the `phpunit-db-profile` CI diagnostic shard.

The shape table is intentionally simple and profile-only. Hash collisions are
acceptable for diagnostic attribution because the sample row is printed with
the hash and this data does not drive runtime decisions.

## Compatibility Impact

No SQL behavior, mysqli API behavior, public C API behavior, storage format,
ownerless lock behavior, or WordPress test selection changes. Existing
unprofiled CI timing shards remain unprofiled and comparable to trunk.

## Directory And Lifecycle Impact

No durable directory-layout changes. Profile rows are transient process output,
and the WordPress timing summary remains a build-report artifact. The database
directory lifecycle and prepared WordPress baseline restore behavior are
unchanged.

## Build, Size, License, And Dependency Impact

The slice adds a small fixed-size profile table to the PHP mysqli extension and
does not add dependencies or license obligations. Unprofiled runs pay only the
existing `php_mylite_mysqli_profile_enabled` branch and no SQL-shape
normalization work.

## Test And Verification Plan

- Build the production PHP embedded target containing
  `mylite_mysqli_php_extension`.
- Run focused PHP extension profile coverage:
  `ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.(api|profile|profile-context)$' --output-on-failure`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run the focused production WordPress `^Tests_DB` diagnostic with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` and verify the timing summary
  contains query-shape rows.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Profile output includes `mylite_mysqli_profile_query_shape_top_count`,
  overflow counters, and top shape hash/calls/ms/verb/sample rows when
  `MYLITE_MYSQLI_PROFILE=1`.
- The focused mysqli profile CTest covers the presence of top-shape rows.
- The WordPress timing summary copies top-shape rows for explicitly profiled
  phases.
- Normal unprofiled WordPress PHPUnit timing shards do not enable this
  diagnostic path.

## Verification Results

Focused local verification completed on the active `ownerless-concurrency`
worktree:

```text
cmake --build --preset php-embedded-prod --target mylite_mysqli_php_extension -j2
ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.(api|profile|profile-context)$' --output-on-failure
bash -n tools/wordpress-phpunit-mysqli-mylite
tools/check-ci-production-builds
```

The focused CTest selector passed all three PHP mysqli extension tests. A
manual profile capture from `mysqli_profile_test.php` printed five normalized
shape rows, including `SELECT BODY FROM PROFILE_NOTES WHERE ID = ?` and
`INSERT INTO PROFILE_NOTES VALUES (?, ?)`.

The focused production WordPress diagnostic command also passed:

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
MYLITE_WORDPRESS_TIMING_LABEL=query-shape-profile-diagnostic \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/query-shape-profile-diagnostic/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB'
```

It ran 651 tests with 3 skips and reported PHPUnit `9.003s`, shell real
`17.133s`, and total harness time `25s`. The timing summary copied the top
shape rows:

```text
query_shape_1: 652 calls, 1432.147 ms, SELECT OPTION_NAME, OPTION_VALUE FROM WPTESTS_OPTIONS WHERE AUTOLOAD IN ( ?, ?, ?, ? )
query_shape_2: 657 calls, 1176.275 ms, SELECT OPTION_VALUE FROM WPTESTS_OPTIONS WHERE OPTION_NAME = ? LIMIT ?
query_shape_3: 651 calls, 859.300 ms, START TRANSACTION;
query_shape_4: 651 calls, 825.298 ms, ROLLBACK
query_shape_5: 31 calls, 166.517 ms, CREATE TABLE WPTESTS_DBDELTA_TEST ...
```

## Risks And Follow-Up

- Top-shape ranking is elapsed-time based and can vary by host load. The output
  is intended for comparing shape families across CI artifacts, not as a stable
  golden ordering.
- The profile does not aggregate shape rows across process-isolated children.
  That is acceptable for the bounded `phpunit-db-profile` shard, which runs as
  one PHPUnit process.
- This slice identifies the next optimization target. It does not itself reduce
  the WordPress PHPUnit wall time.
