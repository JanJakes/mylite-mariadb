# WordPress mysqli Text Result Fast Path

## Problem

The production WordPress PHPUnit non-isolated shard is now split from builds and
uses Release/MinSizeRel guards, so its timings are comparable. The latest green
CI run showed the remaining long shard is real test-body work, not setup:
`wordpress_phpunit_reported_seconds=1066.469` and
`wordpress_phpunit_shell_overhead_seconds=5.042`.

The same shard's mysqli profile attributed about `759683 ms` to query work. The
largest visible buckets were result-query prepare, result-query execute/step,
no-result execution, and cached prepared-statement finalization:

- `query_result_calls=296675`,
- `query_cache_misses=252009`,
- `query_prepare_ms_total=192719.694`,
- `query_result_execute_ms_total=239515.269`,
- `query_result_step_ms_total=235052.742`,
- `query_cache_clear_finalize_calls=251996`,
- `query_cache_clear_ms_total=64614.892`,
- `exec_no_result_ms_total=153620.909`.

The LRU result-statement cache helps exact repeated SQL, but most WordPress
result SQL is unique or falls out of the cache. A previous broad result-query
shortcut through `mylite_exec()` was rejected because the old
SQLite-compatible callback only exposed display column names and would break
`mysqli_fetch_field()` original-name/table metadata.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` `exec_impl()` already executes text SQL
  through `mysql_query()` and drains the first result with `mysql_store_result()`.
- `store_and_emit_result()` already calls `mysql_fetch_fields(result)` to build
  the legacy `column_names` array; the `MYSQL_FIELD` entries also contain
  `org_name`, `table`, and `org_table`.
- `mysql_fetch_lengths(result)` exposes byte lengths for the current stored row,
  which is required before mysqli can safely route binary or embedded-NUL values
  through a text-result path.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  `php_mylite_mysqli_query_impl()` currently routes normal result-producing
  `mysqli_query()` calls through `mylite_prepare()`, `mylite_step()`, and
  `mylite_finalize()` so result metadata can be populated from prepared
  statement column accessors.
- MySQL/MariaDB `mysqli_query()` is a text-query API. Treating `?` as a prepared
  placeholder in that path is less compatible than using text execution.

## Design

Add a new `libmylite` direct-result callback API alongside the existing
`mylite_exec()` API:

- `mylite_exec()` remains source-compatible and keeps its SQLite-like textual
  callback.
- `mylite_exec_result()` executes the same one-shot SQL path but passes each row
  with explicit value lengths plus `mylite_exec_column` metadata containing
  display name, original name, table, and original table.
- The legacy `mylite_exec()` callback becomes a thin adapter over the new
  internal metadata callback.
- A follow-up `mylite_exec_result_with_metadata()` helper emits field metadata
  before row callbacks so zero-row text result sets can still become mysqli
  result objects without falling back to prepared statements.

Switch the mysqli adapter's default result-producing `mysqli_query()` path to
`mylite_exec_result()` for first-seen and non-repeated result SQL. An immediate
exact repeat promotes to the existing prepared-result cache so tight repeated
loops such as `SELECT 1` can still amortize prepare cost. The adapter still
routes no-result DML/DDL through the existing no-result fast path and keeps
`CALL` on direct execution. Preserve the prepared-result cache code behind
`MYLITE_MYSQLI_PREPARED_QUERY_RESULTS=1` as a diagnostic fallback while the
default path uses text-query semantics for unique result SQL.

## Compatibility Impact

The public C API gains an additive function and metadata struct; existing
`mylite_exec()` callers are unchanged.

For mysqli, ordinary `query()` result sets keep `fetch_field()` name,
original-name, table, and original-table metadata while avoiding prepared
statement prepare/reset/finalize cost on unique result-query misses. Immediate
exact repeats may still use prepared statements after the first successful text
execution. Result values are copied with explicit byte lengths, so embedded NUL
values remain covered.

Text-query placeholder behavior becomes more MySQL/MariaDB-compatible:
`mysqli_query($db, 'SELECT ?')` is no longer accepted as an implicit prepared
statement.

Explicit `mysqli::prepare()` and statement execution still use MariaDB prepared
statements.

## Directory And Lifecycle Impact

No durable storage or directory-layout changes. The new API uses the same
direct execution pipeline, ownerless statement locks, page-version read
visibility, and result draining as `mylite_exec()`.

## Build, Size, And Dependency Impact

No dependencies are added. The public header grows one small metadata struct,
one callback typedef, and one exported function. The PHP adapter removes
prepared-result lifecycle work from the default `mysqli_query()` result path.

## Test Plan

- Add embedded C API coverage for `mylite_exec_result()` metadata and
  binary-safe value lengths.
- Add public API misuse validation for `mylite_exec_result()`.
- Extend mysqli API coverage for field metadata, embedded-NUL result values,
  and text-query placeholder rejection.
- Update the mysqli profile CTest assertion to prove the direct result path is
  exercised by default.
- Run focused production C/API, PHP mysqli, WordPress performance, and
  WordPress PHPUnit profile checks.
- Run production CI-build audit, format, and diff checks.

## Acceptance Criteria

- `mylite_exec()` remains compatible with existing callback callers.
- `mylite_exec_result()` returns full field metadata and byte lengths.
- Zero-row text result sets return a result object with field metadata through
  `mylite_exec_result_with_metadata()`.
- Default unique mysqli result queries avoid `mylite_prepare()` while
  preserving field metadata and binary result values.
- `MYLITE_MYSQLI_PREPARED_QUERY_RESULTS=1` can still exercise the old prepared
  result route for diagnostics.
- Production WordPress perf evidence shows result-query prepare/finalize volume
  is sharply reduced on WordPress-shaped result-query workloads.

## Verification Results

Local verification on 2026-06-11 used production build caches:
`build/php-embedded-prod` with Release MyLite,
`build/wordpress-php-embedded-prod` with Release MyLite, and
`build/wordpress-mariadb-embedded` with MinSizeRel MariaDB embedded.

- Focused C/PHP tests passed:
  `ctest --preset php-embedded-prod -R
  '^(libmylite\.api|libmylite\.embedded-exec|php-ext-mysqli-mylite\.(api|profile))$'
  --output-on-failure`.
- The focused mysqli profile test reported `exec_result_calls=9`,
  `query_prepare_calls=1`, and `query_cache_clear_finalize_calls=1`, proving
  unique result SQL used the text-result path while immediate repeats promoted
  to the prepared cache.
- The CI-sized WordPress mysqli `perf-probe` passed with production guards and
  reported `select1_ops_per_second=828.14`, `point_select_ops_per_second=737.92`,
  `insert_autocommit_ops_per_second=771.07`, and
  `insert_direct_autocommit_ops_per_second=726.83` on the local runner.
- Focused production WordPress `Tests_DB` passed `651` tests with `3` skips.
  Compared with the previous LRU profile, prepared result lifecycle counters
  moved from `query_prepare_calls=1602` and
  `query_cache_clear_finalize_calls=1602` to `0` each; the final rerun reported
  `wordpress_phpunit_reported_seconds=10.903` and
  `query_ms_total=6986.259`, compared with the previous
  `wordpress_phpunit_reported_seconds=14.692` and
  `query_ms_total=11354.274` sample.
- The full CI-shaped non-isolated WordPress shard passed locally with `28544`
  tests, `81` warnings, and `90` skips. It reported
  `query_prepare_calls=3865` and `query_cache_clear_finalize_calls=3865`
  versus the prior CI profile's roughly `252k` prepared-result misses/finalizes.
  The local wall clock was not used as a branch/main performance conclusion
  because unchanged no-result execution was much slower on this host than in
  the prior CI sample.
- `cmake --build --preset prod`, `ctest --preset prod -R
  '^(libmylite\.api|tools\.ci-production-builds)$' --output-on-failure`,
  `tools/check-ci-production-builds`,
  `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake
  --build --preset format-check-prod`, and `git diff --check` passed.
