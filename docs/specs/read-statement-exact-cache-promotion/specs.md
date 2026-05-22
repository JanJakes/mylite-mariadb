# Read Statement Exact Cache Promotion

## Problem

Short direct primary-key selects can enter MariaDB's const-table path and call
MyLite through `ha_index_read_idx_map()`. MyLite opens a short read statement
for that storage lookup. The current exact-index cache is built on that read
statement, but `mylite_storage_end_read_statement()` frees the statement without
promoting the cache to the durable thread-local cache.

This means repeated direct point selects can rebuild a complete exact-index
cache every time. A local all-phase storage-smoke sample showed
`direct-pk-selects` at about `5 ms/op` after a separate prepared insert phase
had added unrelated append-history pages, while the isolated phase stayed near
`30 us/op`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_select.cc:24855` calls `join_read_const()` for const-table
  access during optimization.
- `mariadb/sql/sql_select.cc:24984-24988` reads the row with
  `handler::ha_index_read_idx_map()` and `HA_READ_KEY_EXACT`.
- `mariadb/sql/handler.cc:4036-4047` forwards `ha_index_read_idx_map()` to the
  engine's `index_read_idx_map()` without requiring `index_init()`.
- `mariadb/storage/mylite/ha_mylite.cc:2277-2304` routes exact full-key reads
  through `read_exact_unique_index_row_into()` when supported.
- `packages/mylite-storage/src/storage.c::find_exact_index_row_id()` first
  checks an active statement exact-index cache, then builds one for the active
  read statement before falling back to durable lookup paths.
- `packages/mylite-storage/src/storage.c::mylite_storage_commit_statement()`
  already promotes top-level write-statement exact-index and live-row-id caches
  after commit.
- `packages/mylite-storage/src/storage.c::mylite_storage_end_read_statement()`
  closes and frees the read statement without the same promotion.

Profiling evidence:

```text
benchmark_point_selects
  query_uint64
    mylite_exec
      mysql_real_query
        join_read_const
          handler::ha_index_read_idx_map
            ha_mylite::index_read_idx_map
              mylite_storage_find_indexed_row_into
                load_complete_exact_index_cache
                  read_live_index_entries
```

## Scope

- Promote exact-index caches from a top-level read statement into the existing
  durable exact-index cache set when the read statement closes successfully.
- Promote live-row-id caches from a top-level read statement the same way, to
  keep read-statement cache behavior consistent with top-level write-statement
  promotion.
- Add storage test coverage that proves an exact-index cache built under an
  active read statement becomes a durable thread-local cache after close.
- Update roadmap performance notes with the new read-statement cache behavior.

## Non-Goals

- No catalog, page, or file-format change.
- No new public API.
- No storage-level B-tree navigation or append-history compaction.
- No claim that the first read after a cold cache avoids append-history scans.

## Design

`mylite_storage_end_read_statement()` should mirror the safe part of top-level
write-statement promotion:

1. Only promote when the closing read statement has no parent. Nested read
   statements remain scoped to their active read chain.
2. Close the statement through the existing file-cache path.
3. If close succeeds and no same-file write statement or read snapshot is
   active, copy statement exact-index and live-row-id caches to the durable
   thread-local cache sets only when no matching durable cache already exists.
   Read statements do not mutate those caches, so a cache seeded from durable
   storage does not need to be copied back on every close.
4. Let durable cache header checks continue to guard reuse. Durable exact-index
   caches already carry filename, catalog root page, catalog generation, page
   count, table id, index number, and key size.

This keeps write invalidation and table-mutation retargeting on the existing
paths. Same-table writes still discard exact-index caches for the mutated table;
other-table writes update header identity on retained caches.

## Compatibility Impact

No SQL or C API behavior changes. This is a performance and cache-lifetime
change for existing correct exact-index reads.

## Single-File And Embedded-Lifecycle Impact

No new durable file content or companion file. The durable cache is process
local, thread local, and validated against the current `.mylite` header before
reuse.

## Storage Routing Impact

The improvement applies to routed MyLite engine tables, including MariaDB const
table reads for SQL such as `SELECT ... WHERE primary_key = literal`.

## Build, Size, And Dependencies

No dependency or binary-profile change. The code change is limited to the
first-party storage package and test hooks under `MYLITE_STORAGE_TEST_HOOKS`.

## Test Plan

- Add a storage unit test that begins a read statement, performs an exact index
  lookup, ends the read statement, and asserts that a durable exact-index cache
  now exists for the file.
- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev -R 'mylite-storage\.capabilities' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=direct-pk-selects 1000 10000`

## Acceptance Criteria

- Read-statement exact-index caches are not discarded on successful top-level
  read statement close.
- Existing mutation invalidation keeps stale exact-index caches from surviving
  same-table writes.
- The broad all-phase benchmark no longer reports direct primary-key selects in
  the millisecond range after unrelated prepared inserts.
- Focused direct and prepared point-select phases remain correct.

## Risks

- Promoting nested read-statement caches while another read statement is still
  active could expose a snapshot cache too early, so this slice limits promotion
  to top-level read statements.
- The first cold lookup may still scan append history until a navigable index
  root or durable cache exists.
