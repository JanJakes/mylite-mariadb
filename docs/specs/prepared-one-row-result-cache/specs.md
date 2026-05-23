# Prepared One-Row Result And Miss Cache

## Problem

Storage-level primary-key row lookups under a retained read statement are now
well below the routed prepared `SELECT` cost. Local sampling of
`prepared-pk-selects` shows the remaining hot path in MariaDB prepared
execution, especially `Prepared_statement::execute()`,
`mysql_execute_command()`, `JOIN::optimize_inner()`,
`make_join_statistics()`, and `join_read_const_table()`. The handler and
storage path are already using direct exact unique-row materialization, but
MariaDB still re-enters SELECT optimization for each re-execute.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute()` re-enters
  `mysql_execute_command()` for normal prepared execution.
- `mariadb/sql/sql_select.cc::JOIN::optimize_inner()` runs the optimizer and
  calls `make_join_statistics()` even for the prepared point-read shape.
- `mariadb/sql/sql_select.cc::join_read_const_table()` may read the matching
  const row during optimization, which still leaves the SQL-layer optimizer
  cost in every prepared execution.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` and
  `index_read_idx_map()` already use MyLite's direct exact unique-row path for
  supported full-key point reads.
- `packages/libmylite/src/database.cc::begin_prepared_read_scope()` retains one
  MyLite storage read statement across fully drained reset/re-execute loops.

## Design

- Add a `libmylite` prepared result cache for a narrow, deterministic shape:
  simple single-parameter `SELECT <identifier-list> FROM <identifier>
  WHERE <identifier> = ?` statements.
- MariaDB remains the authority for the first execution. MyLite only caches a
  row result after MariaDB returns exactly one row and the caller drains the
  result to `MYLITE_DONE`.
- MyLite also caches deterministic misses after MariaDB returns `MYSQL_NO_DATA`
  for the same simple shape. The miss cache is only reused inside the same
  retained prepared read scope and is invalidated before any write.
- Cache keys are a bound `NULL`, the single bound integer parameter value, or a
  bounded exact bound text/blob byte value. Unsupported and oversized parameter
  kinds use the normal MariaDB path.
- Cached rows are valid only while the retained prepared read statement remains
  open. Cached misses use the same lifetime. Closing that read scope clears the
  cache.
- Writes already close retained prepared read scopes before execution; that
  invalidates cached rows and misses before storage metadata can change.
- Multi-row results, expressions, functions, joins, aliases, ordering, limits,
  locking reads, and variable result shapes stay on the normal MariaDB path.

## Affected Subsystems

- `libmylite` prepared execution and reset/finalize lifecycle.
- MyLite storage read-scope ownership only for cache invalidation; no storage
  file-format or handler behavior changes.

## Compatibility Impact

No SQL-visible behavior changes are intended. The cache is populated only from
MariaDB-produced row or no-row outcomes and only replayed inside the same stable
read snapshot. Statements outside the narrow simple-select classifier continue
through MariaDB execution.

## Single-File And Lifecycle Impact

No durable format, companion-file, locking, journal, or recovery change. The
cache is in-memory, statement-owned, bounded, and dropped with the retained read
statement, statement finalization, or write-before-read-scope close.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

No routing policy change. The fast path only applies after MariaDB has prepared
the statement and produced normal result metadata.

## Binary Size And Dependency Impact

No dependency change. Binary-size impact is limited to a small result-cache
structure and simple SQL-shape classifier in `libmylite`.

## Test And Verification Plan

- Extend prepared routed-select tests with repeated simple point reads, repeated
  no-row point reads, and subsequent writes that must invalidate the retained
  read-scope cache.
- Cover exact bound text keys for row and no-row results without claiming
  broader collation-equivalence reuse across different bound bytes.
- Cover bound `NULL` misses for `=` predicates as deterministic no-row outcomes.
- Cover a row result that is not cacheable before user-side large-value
  materialization, so the miss cache cannot treat a drained non-cacheable row
  as a no-row result.
- Run `git diff --check` and `git clang-format --diff`.
- Build `mylite_embedded_storage_engine_test` and `mylite_perf_baseline`.
- Run `ctest --preset storage-smoke-dev -R libmylite.embedded-storage-engine`.
- Run focused integer, text-key, and miss prepared point-read benchmarks before
  commit.

## Acceptance Criteria

- Repeated simple one-parameter point reads return the same values before a
  write and updated values after a write.
- Repeated simple one-parameter point misses return `MYLITE_DONE`, and a later
  insert for the same key is visible after write invalidation.
- Repeated exact text-key point reads and misses follow the same cache
  invalidation rules as integer keys.
- Repeated bound-`NULL` equality misses return `MYLITE_DONE`.
- A non-cacheable row result is not converted into a cached miss after drain.
- Reset-before-drain and finalization do not call MariaDB result cleanup for a
  cache-served row.
- Prepared point-select benchmarks improve materially without changing storage
  tests.
- The performance harness can measure bounded exact text-key cache hits
  separately from integer primary-key cache hits, with row preparation and cache
  warm-up reported outside the hot component loop.
- Non-matching SQL stays on the normal MariaDB path.

## Risks And Open Questions

- This is deliberately a cache, not a general optimizer bypass. It helps hot
  repeated point reads with recurring keys, but cold unique-key streams still
  pay MariaDB execution cost.
- The simple SQL classifier is conservative. Broader deterministic prepared
  SELECT shapes need separate source-linked slices.
