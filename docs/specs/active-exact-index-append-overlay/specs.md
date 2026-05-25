# Active Exact Index Append Overlay

## Problem Statement

Prepared insert throughput currently improves for short runs but degrades for
larger runs. The hot path is duplicate-key probing for fixed-width unique keys:
`ha_mylite::write_row()` calls `mylite_check_duplicate_keys()`, which calls
`mylite_storage_find_index_entry()` for raw exact keys.

The storage exact-index cache avoids repeated scans, but active statements seed
their cache by copying the complete durable exact-index cache. In autocommit
insert loops this can copy a growing keyset into a fresh statement cache for
each row, creating O(n) memory work per insert before the row append itself.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), as recorded in
  `docs/architecture/engineering-standards.md`.
- MyLite handler path:
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` prepares index
  entries, advances autoincrement where needed, then calls
  `mylite_check_duplicate_keys()` before appending the row.
- Duplicate-key path:
  `mariadb/storage/mylite/ha_mylite.cc::mylite_check_duplicate_keys()` uses
  `mylite_storage_find_index_entry()` for non-null fixed-width raw exact unique
  keys.
- Storage path:
  `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  resolves the table and delegates to `find_exact_index_row_id()`.
- Cache behavior:
  `find_exact_index_row_id()` first checks the active statement exact-index
  cache. If no active cache exists, `load_cached_exact_index_entry_in_statement()`
  creates one and seeds it by copying the durable cache or by loading the full
  index. Successful appends then call `append_active_exact_index_cache_entries()`,
  and top-level statement commit promotes active exact-index caches back to the
  durable cache.

## Proposed Design

Add a separate active append-overlay mode for exact-index caches:

- Durable exact-index caches remain the complete committed keyset for their
  header generation.
- During an active append-only statement, duplicate probes may use the durable
  cache as the committed base without copying it into the statement cache.
- The active statement cache records only keys appended by the statement.
- Active cache hits are always useful. Active cache misses are authoritative
  only when the cache is known to contain a complete exact keyset.
- If an active statement mutates a table through update/delete while holding an
  incomplete exact-index overlay, clear the active exact-index caches and fall
  back to the existing complete scan/load behavior.
- On commit, preserve and retarget only the durable exact-index caches that have
  matching append overlays, then merge the appended keys. Other same-table
  durable exact-index caches are invalidated as they are today.

## Affected Subsystems

- MyLite storage exact-index cache metadata and lookup.
- Active statement commit cache promotion.
- Duplicate-key probe fast path used by the MyLite MariaDB handler.

## Compatibility Impact

The change is internal and should not change SQL-visible duplicate-key,
rollback, or lookup behavior. It preserves MySQL/MariaDB duplicate-key
semantics by keeping committed durable rows and same-statement appended rows
visible to duplicate probes.

## DDL Metadata Routing Impact

No DDL metadata format or routing behavior changes.

## Single-File And Embedded-Lifecycle Impact

No file-format change. The overlay is thread-local runtime cache state only and
does not add durable or transient companion files.

## Public API Or File-Format Impact

No public API or file-format impact.

## Storage-Engine Routing Impact

No routing policy change. The optimization applies after the handler has routed
the table to MyLite storage.

## Wire-Protocol Or Integration-Package Impact

No impact.

## Binary-Size Impact

Small first-party storage code increase only. No dependency or MariaDB build
profile change.

## License Or Dependency Impact

No new dependency or license impact.

## Test And Verification Plan

- Add a storage regression test proving that an active statement can:
  - use a durable exact-index cache for committed rows without copying it,
  - see an appended row through the active overlay in the same statement,
  - commit with the durable cache still correct for both old and new keys.
- Add a rollback/mutation-sensitive assertion if needed while implementing.
- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|embedded-storage-engine' --output-on-failure`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `git diff --check`
- Benchmark:
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 50000`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

## Acceptance Criteria

- Exact duplicate probes remain correct for committed rows and same-statement
  appended rows.
- Active append-only statements no longer copy the complete durable exact-index
  cache per row when a matching durable cache already exists.
- Same-table durable exact-index caches without a matching active overlay are
  still invalidated on commit.
- Existing storage and embedded storage-engine tests pass.
- Prepared insert component timing no longer shows the previous severe 50k-row
  scaling regression.

## Risks And Unresolved Questions

- Update/delete statements can invalidate append-overlay assumptions; the
  implementation must clear incomplete active exact-index caches before falling
  back to the conservative path.
- This slice optimizes the exact-key duplicate probe path. It does not replace
  planned production B-tree split/merge/navigation work.
