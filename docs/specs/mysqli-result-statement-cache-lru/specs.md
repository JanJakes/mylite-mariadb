# mysqli Result Statement Cache LRU

## Problem

Production WordPress `Tests_DB` profiling after the fully-drained reset fast
path still shows many result-query cache misses and only a few hits. The
adapter keeps one prepared result statement per mysqli link, so interleaved
exact repeated `SELECT` statements evict each other and pay repeated
`mylite_prepare()` and `mylite_finalize()` cost. A direct `mylite_exec()`
shortcut for result-query misses would avoid prepared lifecycle cost, but the
current callback API only exposes display column names while the prepared path
preserves original column and table metadata used by `mysqli_fetch_field()`.

## Source Findings

- Target base: MariaDB 11.8.6, initial import
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` routes
  result-producing `mysqli_query()` calls through `mylite_prepare()` so result
  metadata can be populated from `mylite_column_name()`,
  `mylite_column_org_name()`, `mylite_column_table()`, and
  `mylite_column_org_table()`.
- The existing `mylite_exec()` callback shape in `mylite.h` is
  SQLite-compatible and exposes only `values` and `column_names`, so it is not
  a drop-in replacement for mysqli result queries that inspect field metadata.
- Existing adapter invalidation clears the result statement cache before
  `CALL`, explicit prepared statements, reconnect, close, DDL, schema,
  transaction, lock, `SET`, `USE`, and error paths. Ordinary no-result
  `INSERT`, `UPDATE`, `DELETE`, and `REPLACE` statements without `RETURNING`
  preserve the result cache.

## Design

Replace the one-entry mysqli result statement cache with a fixed-size exact-SQL
LRU cache. Each entry owns one `mylite_stmt`, one SQL string, and a use tick.
Lookup compares SQL bytes exactly. A hit resets the matched prepared statement,
updates the entry's use tick, and re-executes it. A miss prepares a new
statement and stores it in a free entry or evicts the least recently used entry.

Keep the existing conservative invalidation points and profile counters. Cache
clear finalizes every live entry. Cache miss counts still mean no exact
prepared statement was found. Cache hit counts now include non-consecutive
exact repeats that remain within the bounded LRU.

Do not normalize SQL, parameterize literals, or route result-query misses
through `mylite_exec()` in this slice.

## Compatibility Impact

The mysqli query result shape stays on the prepared path, preserving field
metadata compatibility. The change only retains more already-prepared exact SQL
statements per link. Schema and session-state changes keep the current broad
cache clear behavior.

## Lifecycle And Storage Impact

No durable storage or directory-layout changes. Link lifetime owns the cached
statements; close, reconnect, and object destruction finalize every cache entry
before closing the `mylite_db` handle.

## Test Plan

- Extend the mysqli profile test with interleaved repeated SELECT statements
  and assert the profile sees cache hits beyond the old one-entry behavior.
- Run the focused PHP mysqli profile/API CTest selector under
  `php-embedded-prod`.
- Run the CI production-build audit and formatting/diff checks.
- Run a focused production WordPress `Tests_DB` profile to see whether real
  WordPress query order benefits from the larger exact-SQL cache.

## Acceptance Criteria

- Interleaved exact repeated SELECT statements reuse cached prepared metadata.
- Existing DML-preserved result-cache behavior still observes current rows.
- DDL and explicit prepared statements still clear every cached result
  statement.
- The cache remains bounded and owned by the mysqli link.

## Verification Evidence

- `php-ext-mysqli-mylite.profile` now covers alternating exact SELECT
  statements and expects `mylite_mysqli_profile_query_cache_hits=4`, which
  includes two non-consecutive cache hits that the old one-entry cache would
  miss.
- A focused production WordPress `Tests_DB` profile reported
  `query_cache_hits=13`, `query_cache_misses=1602`,
  `query_prepare_calls=1602`, `query_cache_clear_finalize_calls=1602`,
  `query_ms_total=11354.274`, and
  `wordpress_phpunit_reported_seconds=14.692`.
- The pre-LRU fast-reset profile reported `query_cache_hits=3`,
  `query_cache_misses=1612`, `query_prepare_calls=1612`,
  `query_cache_clear_finalize_calls=1612`, `query_ms_total=13141.702`, and
  `wordpress_phpunit_reported_seconds=16.442`.

## Risks

The optimization only helps exact SQL repeats within the cache capacity. It
does not help point-select loops where literal values differ every time, and it
does not reduce MariaDB process startup/shutdown cost.
