# Query Cache Trim

## Problem

The default embedded profile still builds MariaDB's query cache runtime through
`mariadb/sql/sql_cache.cc` and, for `libmysqld`, the embedded result
serializer in `mariadb/libmysqld/emb_qcache.cc`. Query cache is a
server-global result cache for repeated `SELECT` statements, not durable
application state and not part of MyLite's file-owned embedded contract.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB documentation describes the query cache as a cache of `SELECT`
  result sets, disabled by default because it does not scale well on high
  throughput multicore workloads, and reports build availability through
  `have_query_cache`.
  See <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/buffers-caches-and-threads/query-cache>.
- `mariadb/sql/sql_cache.cc` implements the global `Query_cache` object,
  memory pool, result storage, invalidation, flush, resize, and query lookup
  paths.
- `mariadb/sql/sql_cache.h` exposes `Query_cache`, cache macros such as
  `query_cache_store_query()` and `query_cache_send_result_to_client()`,
  status counters, and `Query_cache_query_flags`.
- `mariadb/sql/sql_parse.cc` calls
  `query_cache_send_result_to_client()` before parsing and
  `query_cache_store_query()` around `SELECT` execution. These calls must
  remain safe no-ops when the cache is disabled.
- `mariadb/sql/sql_reload.cc` implements `FLUSH QUERY CACHE` and
  `RESET QUERY CACHE` through `query_cache.pack()` and
  `query_cache.flush()`.
- `mariadb/sql/sys_vars.cc` exposes `query_cache_size`,
  `query_cache_limit`, `query_cache_min_res_unit`, `query_cache_type`,
  `query_cache_wlock_invalidate`, `query_cache_strip_comments`, and
  `have_query_cache`.
- `mariadb/sql/mysqld.cc` initializes the query cache during startup, reports
  `have_query_cache=YES`, and exposes `Qcache_*` status counters from the
  global `query_cache` object.
- `mariadb/libmysqld/emb_qcache.cc` serializes and deserializes embedded
  result sets into query-cache result blocks. It is only referenced by the
  full query-cache implementation.
- `mariadb/sql/sql_yacc.yy` parses `SQL_CACHE` and `SQL_NO_CACHE` select
  hints independently from the cache implementation. Keeping those hints as
  no-op syntax preserves compatibility with applications that include them.

## Design

- Add `MYLITE_WITH_QUERY_CACHE_RUNTIME`, defaulting to upstream-compatible
  `ON` and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When disabled, build a MyLite-owned `mylite_query_cache_disabled.cc` instead
  of `sql_cache.cc`.
- Omit `emb_qcache.cc` from `libmysqld` when the query cache runtime is
  disabled.
- The disabled `Query_cache` implementation:
  - keeps public status counters at zero,
  - reports disabled state from `is_disabled()`,
  - returns zero from resize and cache-size setters,
  - makes lookup, store, invalidate, flush, pack, insert, abort, and end-of-
    result paths no-ops,
  - keeps table key helpers available for retained handler code,
  - avoids allocating query-cache memory or locks.
- Guard MariaDB startup so the disabled profile does not auto-enable
  `query_cache_type` from the sysvar default size and reports
  `have_query_cache=NO`.
- Reject `FLUSH QUERY CACHE`, `RESET QUERY CACHE`, and query-cache sysvar
  assignments through the public `libmylite` SQL policy. These are
  administrative cache-management surfaces, not embedded file-owned behavior.
- Keep `SELECT SQL_CACHE ...` and `SELECT SQL_NO_CACHE ...` accepted as
  no-op compatibility hints.

## Compatibility Impact

Query-cache storage and administration become explicitly unsupported. Ordinary
`SELECT`, DDL, DML, and SELECT cache-hint syntax remain available. The public
runtime reports `have_query_cache=NO` so applications can detect the omission.

## Single-File And Embedded Lifecycle Impact

No durable file-format change. The trim removes server-global cache state from
startup and repeated open/close cycles. It does not add sidecar files or new
runtime companions.

## Storage-Engine Routing Impact

No storage-routing change. Query-cache invalidation hooks become no-ops while
the routed MyLite storage engine remains responsible for table reads, writes,
and metadata.

## Public API Impact

No C API surface changes. `mylite_exec()` and `mylite_prepare()` return stable
unsupported-surface diagnostics for query-cache administration SQL.

## Binary-Size Impact

This slice should remove `sql_cache.cc.o` and `emb_qcache.cc.o` from the
default embedded archive. The expected effect is measured by the embedded build
wrapper and size report rather than assumed from the historical research table.

## License And Dependency Impact

No new dependency. New stub code remains GPL-2.0-compatible as part of the
MariaDB-derived tree.

## Test And Verification Plan

- Add direct SQL tests for `have_query_cache=NO`, `query_cache_type=OFF`,
  `query_cache_size=0`, rejected `FLUSH QUERY CACHE`, rejected
  `RESET QUERY CACHE`, rejected query-cache sysvar assignment, no-op accepted
  `SELECT SQL_CACHE`, and no-op accepted `SELECT SQL_NO_CACHE`.
- Add prepared-statement rejection tests for representative query-cache
  administrative SQL.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Verify the embedded archives contain `mylite_query_cache_disabled.cc.o` and
  do not contain `sql_cache.cc.o` or `emb_qcache.cc.o`.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` CMake build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile links the query-cache stub, not the full query
  cache runtime.
- `have_query_cache` reports `NO`; `query_cache_type` and `query_cache_size`
  report disabled values.
- Administrative query-cache SQL is rejected by direct and prepared public APIs
  with MyLite diagnostics.
- Ordinary SELECT statements and SELECT cache hints still execute.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- `Query_cache` is referenced from common parser, DDL, DML, and handler code,
  so the stub must preserve the symbol and method surface even when methods are
  no-ops.
- Query-cache sysvars are still registered by MariaDB. Public MyLite policy
  must reject cache-management assignments to avoid reporting a configurable
  feature that is compiled out.
