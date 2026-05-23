# Full Index Entryset Cache

## Problem

Prepared text-key point reads cannot safely use MyLite's raw exact byte-key
lookup, because MariaDB owns string and collation equality. The handler
therefore builds a full index cursor with
`mylite_storage_read_index_entries()` and compares the key tuple through
MariaDB. That is correct, but repeated prepared executions over the same read
snapshot rebuild the full live index entryset from storage each time.

The prepared text-select benchmark exposes this as expensive cache warm-up:
the first pass over many distinct text keys repeatedly scans the same index
pages before the prepared one-row result cache can help later repeated keys.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  routes raw exact non-null integer keys through
  `mylite_storage_find_index_entry()` or
  `mylite_storage_read_exact_index_entries()`. Non-raw keys use
  `mylite_storage_read_index_entries()` so the handler can compare key tuples
  with MariaDB semantics.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_uses_raw_exact_filter()`
  currently limits raw exact filters to integer-like field types. This is a
  deliberate compatibility boundary for collation-sensitive strings.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  can use published leaf roots, but otherwise calls
  `read_live_index_entries()` for every full-index read.
- `packages/mylite-storage/src/storage.c` already has active read-statement and
  durable exact-index cache sets that store complete fixed-size key images and
  row ids, with statement promotion and mutation invalidation.

## Design

- Keep non-raw SQL key comparison in the MariaDB handler. Do not broaden
  `mylite_key_uses_raw_exact_filter()` to string or binary types in this slice.
- Teach `mylite_storage_read_index_entries()` to reuse a complete fixed-size
  exact-index cache for full-index reads when one exists for the same file
  header, table id, and index number.
- Mark exact-index caches that contain a complete entryset, and require that
  marker for full-index reuse. Exact lookup caches with a caller-selected key
  width must not satisfy a full-index read unless the loaded entryset proved it
  has that fixed width.
- When a full-index read falls through to the existing leaf/full live scan and
  the resulting entryset has one fixed key size, seed either:
  - the active read-statement cache when one owns the file view; or
  - the durable thread-local exact-index cache when the read is outside an
    active mutation/snapshot boundary.
- Leave active write checkpoints on the existing full-scan path. They already
  maintain exact caches for mutation-local exact lookup paths, and complete
  full-index cache promotion across failed DML rollback has a higher
  correctness cost than this read-path slice needs.
- Reuse existing exact-cache invalidation, retargeting, and read-statement
  promotion. The cache stores storage key images only; MariaDB still filters and
  orders cursor rows.
- Leave variable-size entrysets uncached. They keep the existing scan path.

## Compatibility Impact

No SQL-visible behavior change is intended. For non-raw keys, the storage cache
only avoids rebuilding the candidate entryset; MariaDB still decides equality,
prefix, and ordering semantics through the existing handler comparison path.

## Single-File And Lifecycle Impact

No file-format, journal, locking, or companion-file change. The cache is
in-memory and scoped to existing active read statements or durable thread-local
cache lifetimes.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` format change.

## Binary-Size And Dependency Impact

No dependency change. The implementation adds small first-party storage helpers
around the existing exact-index cache.

## Test And Verification Plan

- Add storage unit coverage showing that `mylite_storage_read_index_entries()`
  seeds an active read-statement exact-index cache for fixed-size entries and
  promotes it to the durable cache on read-statement close.
- Add storage unit coverage showing durable full-index reads reuse the same
  cache and that updates invalidate or retarget it through existing mutation
  hooks.
- Add regression coverage showing a non-complete exact cache for the same index
  does not satisfy full-index reads.
- Run `cmake --build --preset dev --target mylite_storage_test`.
- Run `ctest --test-dir build/dev -R mylite-storage --output-on-failure`.
- Build and run the storage-smoke benchmark phase
  `prepared-text-select-components` to verify warm-up cost falls while hot
  cache-hit costs remain valid.
- Run `git diff --check` and `git clang-format --diff`.

## Acceptance Criteria

- Full-index reads over fixed-size key images create and reuse an exact-index
  cache without changing returned live entries.
- Active read-statement full-index caches promote to durable caches on close.
- Existing exact lookup, mutation invalidation, leaf-root, and storage-engine
  smoke tests pass.
- Prepared text-select benchmark warm-up is materially lower than repeated full
  storage scans, while non-raw string equality remains MariaDB-owned.

## Verification Results

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-text-select-components 1000 100000`

On this local machine, the 1000-row prepared text-key warm-up fell from roughly
3575 ms before this slice to 39 ms after this slice, and the hot row component
remained about 0.388 us/op.

## Risks And Open Questions

- This is still a cache over complete entrysets, not a navigable collation-aware
  string index. It reduces repeated scans but does not make cold first lookup
  SQLite-like.
- Very large indexes can still allocate large entryset caches, matching the
  existing exact-index cache memory tradeoff. A future pager/B-tree slice should
  replace this for broad workloads.
