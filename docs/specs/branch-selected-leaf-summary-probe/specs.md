# Branch Selected-Leaf Summary Probe

## Problem

Prepared inserts that route through branch maintenance already cache active
leaf pages in the current statement. Selected-leaf planning still copied a full
cached page through `read_branch_insert_plan_leaf_page()` even when the planner
only needed the leaf table id, index number, key size, and entry count. Local
sampling of the prepared-insert hot path after packed-tail memoization showed
that cached page copy work was still visible below branch insert planning.

## Source References

- MariaDB base: `mariadb-11.8.6` at
  `9bfea48ac3ff9879ff12fba9ea82032e324fc434`.
- MyLite handler write path:
  `mariadb/storage/mylite/ha_mylite.cc::write_row()`.
- MyLite maintained branch planning:
  `packages/mylite-storage/src/storage.c::plan_branch_index_root_insert()`,
  `plan_level_two_branch_index_root_insert()`,
  `plan_level_three_branch_index_root_insert()`,
  `plan_level_four_branch_index_root_insert()`, and
  `plan_deep_branch_index_root_insert()`.
- Existing summary cache precedent:
  `read_branch_leaf_range_plan_scan_leaf()` and
  `read_cached_active_index_leaf_page_summary_from_cache()`.

## Design

Introduce `read_branch_insert_plan_leaf_summary()` for selected-leaf planning.
It first probes the active leaf-page cache metadata without copying the cached
page payload. On a cache miss, it keeps the existing durable path: read the
page, decode it, validate the expected table/index/key shape, store the active
leaf cache entry, and return only the summary fields that the planner uses.

The branch planners continue to make the same split and redistribution
decisions from `entry_count` and key shape. Writer paths still reread protected
pages before rewriting them, so this optimization does not replace durability
validation or change the rollback-journal write set.

## Compatibility

No SQL or public C API behavior changes. The helper preserves the existing
corruption checks for mismatched table id, index number, or key size. It only
reduces redundant transient copies when repeated same-statement insert planning
already has an active leaf-cache entry.

## Tests

- `mylite_storage_test_branch_insert_plan_uses_active_summary_cache()` covers
  a cached selected-leaf summary hit and asserts that no level-`2` branch leaf
  plan read is counted.
- Existing maintained branch insert, branch range redistribution, and storage
  smoke tests cover fallback reads and writer behavior.

## Acceptance Criteria

- Selected-leaf branch insert planners use the summary helper instead of the
  full cached-page reader.
- Cache misses still read, decode, validate, and publish the active leaf cache.
- Same-statement cached selected-leaf planning avoids a page read and full page
  copy when only summary metadata is required.
