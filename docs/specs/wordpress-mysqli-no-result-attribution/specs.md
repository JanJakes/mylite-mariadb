# WordPress mysqli No-Result Attribution

## Problem Statement

The WordPress PHPUnit timing split now makes build, setup, probe, and test-only
durations visible under production build guards. The remaining performance
question is inside the mysqli query body. The result-query fast path removed
most prepared-result lifecycle churn, but the existing profile still reports
no-result SQL as one coarse `exec_no_result_ms_total` bucket.

Before changing execution behavior, MyLite needs evidence for whether that
bucket is dominated by MariaDB `mysql_query()`, status capture, result draining,
current-schema bookkeeping, or PHP adapter status synchronization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current branch reference before this slice: `80b87f89`
  (`Stage WordPress REST fixture asset`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c:3451` implements
  `php_mylite_mysqli_exec_no_result_query_impl()`. It increments
  `exec_no_result_calls`, times one `mylite_exec()` call, then separately times
  public mysqli status-property synchronization.
- `packages/libmylite/src/database.cc:4447` implements `exec_result_impl()`,
  the shared direct text-query path behind `mylite_exec()`,
  `mylite_exec_result()`, and `mylite_exec_result_with_metadata()`.
- The ordinary non-ownerless path at
  `packages/libmylite/src/database.cc:4474` runs pending ownerless cleanup,
  optional plain-read setup, `mysql_query()`, `mysql_affected_rows()`,
  `mysql_insert_id()`, `store_and_emit_result()`,
  `update_current_schema_after_successful_sql()`, and handle status updates.
- The ownerless path at `packages/libmylite/src/database.cc:4504` has more
  statement-boundary policy and locking work around the same native
  `mysql_query()`, affected-row/insert-id capture, result-drain, schema update,
  and handle status-update phases.
- MariaDB declares `mysql_affected_rows()` and `mysql_insert_id()` in
  `mariadb/include/mysql.h:430`, `mysql_query()` in
  `mariadb/include/mysql.h:480`, and `mysql_store_result()` in
  `mariadb/include/mysql.h:499`.
- MariaDB implements buffered `mysql_store_result()` in
  `mariadb/sql-common/client.c:3677`. It returns `NULL` when no result fields
  exist, and otherwise buffers rows, moves field ownership into the result, and
  updates `mysql->affected_rows`.
- MariaDB implements `mysql_affected_rows()` as a direct `mysql->affected_rows`
  read at `mariadb/sql-common/client.c:3772`.

## Design

Add a libmylite direct-execution performance counter block that is disabled by
default and enabled only by the existing `MYLITE_MYSQLI_PROFILE=1` PHP mysqli
profile path.

The counters are intentionally diagnostic exports from `mylite.so`, not public
header API:

- total direct `exec_result_impl()` calls;
- native `mysql_query()` elapsed time and native query error count;
- affected-row and insert-id capture elapsed time;
- `store_and_emit_result()` elapsed time;
- result-set and no-result-set counts;
- current-schema update elapsed time;
- final handle status-update elapsed time.

The PHP mysqli extension resets and enables those counters during module
initialization when `MYLITE_MYSQLI_PROFILE=1`, disables and reads them during
module shutdown, and prints them as additional `mylite_mysqli_profile_*` rows.
Unprofiled runs do not call `clock_gettime()` in the new libmylite path.

The WordPress harness appends the most useful new rows to the existing timing
summary and aggregate profile summary when profiling is explicitly enabled.

## Compatibility Impact

No SQL behavior, MySQL/MariaDB API behavior, PHP mysqli API behavior, public
`libmylite` header API, native storage behavior, directory layout, or ownerless
locking semantics change.

The new symbols are diagnostic-only implementation details used by the bundled
PHP extension. They are not declared in `mylite.h` and are not compatibility
claims for external applications.

## Directory And Lifecycle Impact

No durable files are created or moved. The counters are process-local atomics in
libmylite and are reset for the PHP process when profiling starts.

## Native Storage And Ownerless Impact

Native storage execution is observed but not changed. Ownerless statements get
the same common native-query/result/status attribution, while existing
ownerless-specific performance counters remain the source of deeper
ownerless-lock, page-version, redo, checkpoint, and reclaim attribution.

## Build, Size, License, And Dependencies

No dependency or license impact. The compiled libraries gain a small diagnostic
counter block and a few conditional monotonic-clock samples that run only when
profiling is enabled.

## Test And Verification Plan

- Build the production PHP mysqli extension target.
- Run `php-ext-mysqli-mylite.profile` under `php-embedded-prod` and require a
  nonzero libmylite direct-execution profile key.
- Run the API/profile focused PHP mysqli CTest subset.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds` and the production CTest guard.
- Run the production format check and `git diff --check`.
- Check the current CI run for the REST fixture commit while this slice is in
  progress.

## Acceptance Criteria

- Existing mysqli profile keys remain present and unchanged.
- A profiled PHP mysqli run emits nonzero
  `mylite_mysqli_profile_libmylite_exec_result_calls`.
- Profile output includes direct-execution `mysql_query`, result drain,
  result/no-result, schema-update, and handle-status-update counters.
- The WordPress timing summary can expose the new keys for profiled diagnostic
  runs.
- Unprofiled production CI timing remains on the lean path.

## Verification Results

Local verification on 2026-06-18 used `build/php-embedded-prod` with Release
MyLite and a fresh production-guarded MariaDB embedded archive.

- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension -j2` passed.
- `cmake --build --preset prod -j2` passed after the diagnostic helper
  functions were limited to embedded builds so non-embedded production builds
  do not fail `-Werror=unused-function`.
- `ctest --preset php-embedded-prod -R
  '^php-ext-mysqli-mylite\.profile$' -V` passed. The profile emitted
  `mylite_mysqli_profile_libmylite_exec_result_calls=14`,
  `mylite_mysqli_profile_libmylite_exec_result_mysql_query_ms_total=35.781`,
  `mylite_mysqli_profile_libmylite_exec_result_store_result_ms_total=0.257`,
  `mylite_mysqli_profile_libmylite_exec_result_result_sets=9`,
  `mylite_mysqli_profile_libmylite_exec_result_no_result_sets=5`,
  `mylite_mysqli_profile_libmylite_exec_result_current_schema_ms_total=0.030`,
  and
  `mylite_mysqli_profile_libmylite_exec_result_status_update_ms_total=0.001`.
- `ctest --preset php-embedded-prod -R
  '^php-ext-mysqli-mylite\.(api|profile|profile-context)$'
  --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R
  '^(libmylite\.api|libmylite\.embedded-exec|php-ext-mysqli-mylite\.(api|profile|profile-context))$'
  --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R
  '^(libmylite\.ownerless-primitives|libmylite\.ownerless-cross-process-sql\.0|libmylite\.ownerless-single-owner-history-wal-proof)$'
  --output-on-failure` passed.
- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.
- `nm -D build/php-embedded-prod/packages/php-ext-mylite/mylite.so` showed the
  diagnostic `mylite_exec_result_perf_{set_enabled,reset,read}` symbols
  exported for the bundled PHP mysqli extension.

## Risks And Follow-Up

- The attribution covers common direct-execution phases. It does not replace
  the existing ownerless-native performance counters for deeper page-log,
  redo, checkpoint, lock, and reclaim phases.
- This slice is not yet the optimization. The follow-up optimization should use
  the new profile rows to choose between MariaDB execution, result-drain,
  schema/status bookkeeping, or broader query-shape work.
