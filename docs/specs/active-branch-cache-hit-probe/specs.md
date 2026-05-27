# Active Branch Cache Hit Probe

## Problem

Prepared insert sampling after the active leaf-cache and left-range planning
work still shows `read_cached_active_index_branch_page()` on the hot path. Some
callers need a mutable page copy, but
`maintained_index_roots_allow_packed_insert()` only needs to know whether a
branch root is already present in the active branch-page cache. It currently
copies the full `4096`-byte branch page and decodes cached metadata just to
continue to the next index.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`;
  - `mariadb/sql/handler.cc:handler::ha_write_row()`;
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`.
- `packages/mylite-storage/src/storage.c::maintained_index_roots_allow_packed_insert()`
  calls `read_cached_active_index_branch_page()` but ignores the copied page
  and decoded branch page on cache hits.
- `read_cached_active_index_branch_page()` must keep copying for writer paths
  and other callers that require local mutable page bytes.

## Design

Add a cache-hit-only active branch-page probe that validates the requested page
id, resolves the root active branch cache, and checks for a matching cached
entry without copying page bytes. Use it in
`maintained_index_roots_allow_packed_insert()` before falling back to durable
page reads.

## Non-Goals

- No read-only branch-page view for planning yet; callers that need branch
  payload bytes keep using the existing copy helper.
- No cache ownership, eviction, page format, storage-routing, or SQL behavior
  change.

## Compatibility And Lifecycle Impact

No MySQL/MariaDB compatibility change. The helper only observes transient
statement-owned cache state and leaves durable `.mylite` file, journal, lock,
and companion-file behavior unchanged.

## Test And Verification Plan

- Extend active branch page cache test-hook coverage to assert that the new hit
  probe detects cached and missing branch pages without requiring a page buffer.
- Run:
  - `git diff --check`;
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`;
  - `cmake --build --preset dev --target mylite_storage_test`;
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`;
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`;
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`;
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`.

## Acceptance Criteria

- `maintained_index_roots_allow_packed_insert()` no longer copies cached branch
  root pages when it only needs a cache hit decision.
- Existing mutable branch-page cache readers remain unchanged.
- Storage and embedded storage-engine tests pass.

## Verification Results

The committed-tree 100k prepared-insert component run reported `25.115 us/op`
for the prepared insert step, `52.988 ms` for the prepared insert commit,
`31,653,888` final bytes, and `7,728` header pages. The file shape stayed
unchanged; the commit timing was noisy on this local run.

## Risks

- The helper must preserve the same page-id corruption behavior as the existing
  cached branch reader. It uses the same `is_addressable_page_id()` guard.
