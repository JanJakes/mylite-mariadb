# Branch Range Active Leaf Cache Handle

## Problem

After branch range planning switched to metadata-only active leaf-cache probes,
fresh prepared-insert samples still show
`read_cached_active_index_leaf_page_summary()` and active leaf cache lookup work
under both rightward and leftward redistribution scans. Each scanned sibling
leaf currently resolves the active root statement from the `FILE *` before
probing the same active leaf cache.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`;
  - `mariadb/sql/handler.cc:handler::ha_write_row()`;
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`.
- `packages/mylite-storage/src/storage.c::try_plan_branch_leaf_range_insert_redistribution()`
  calls the right and left range scanners for the same `FILE *` and active
  statement context.
- `read_branch_leaf_range_plan_scan_leaf()` currently resolves the active leaf
  cache through `read_cached_active_index_leaf_page_summary()` for every
  candidate sibling.
- Cache misses then store the decoded leaf through
  `store_active_index_leaf_page_for_file()`, which repeats the same active
  statement resolution.

## Design

Resolve the active leaf-page cache once at the start of a branch range planning
attempt and pass the cache pointer into right, left, and per-leaf scan helpers.
Split the active leaf-page store helper so callers that already have the cache
can store directly, while existing file-based callers keep their current API.

## Non-Goals

- No cache lookup algorithm, capacity, eviction, page format, or durability
  change.
- No redistribution policy, writer, public API, storage-routing, or SQL behavior
  change.

## Compatibility And Lifecycle Impact

No MySQL/MariaDB compatibility change. The cache pointer belongs to the same
root active statement that the existing per-leaf helper would resolve. Durable
file, journal, lock, and companion-file behavior are unchanged.

## Test And Verification Plan

- Keep branch leaf-range planning, rollback, and level-two redistribution tests
  passing.
- Run:
  - `git diff --check`;
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`;
  - `cmake --build --preset dev --target mylite_storage_test`;
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`;
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`;
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`;
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`.

## Acceptance Criteria

- Branch leaf-range planning resolves the active leaf cache once per range
  planning attempt instead of once per candidate sibling.
- Cache hits and misses retain the same validation, read, decode, and cache-store
  behavior.
- Existing storage and embedded storage-engine tests pass.

## Verification Results

The committed-tree 100k prepared-insert component run reported `25.078 us/op`
for the prepared insert step, `75.465 ms` for the prepared insert commit,
`31,653,888` final bytes, and `7,728` header pages. The file shape stayed
unchanged; commit timing remained noisy on this local run.

## Risks

- The hoisted cache pointer must not cross a cache invalidation boundary. The
  range planner does not perform rollback, commit, or cache-clearing operations
  while scanning candidates, so the active statement cache owner remains stable
  for the planning attempt.
