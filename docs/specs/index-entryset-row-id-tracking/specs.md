# Index Entryset Row ID Tracking

## Problem

Complete exact-index cache builds over append-history index pages call
`remove_index_entries_by_row_id()` before appending each live index entry. That
is correct for replacement history, but it turns the common append-only case
into an O(n²) entryset build because each removal scans the growing entryset.

A local prepared-update profile with a larger table sampled almost entirely in:

```text
load_complete_exact_index_cache
  read_live_index_entries
    read_live_index_entries_from
      remove_index_entries_by_row_id
```

This matters because prepared updates perform an exact primary-key lookup before
the row mutation. Larger tables can spend most of the first exact-cache build in
the defensive removal loop.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc` executes single-table updates through the normal
  optimizer and handler read/update flow.
- The sampled prepared update path enters
  `QUICK_EXACT_KEY_SELECT::get_next()`, then
  `handler::ha_index_read_map()`, then MyLite's `ha_mylite::index_read_map()`.
- `mariadb/storage/mylite/ha_mylite.cc:2295-2304` applies the direct exact
  unique lookup path for full-key exact reads before falling back to a cursor.
- `packages/mylite-storage/src/storage.c::read_live_index_entries_from()`
  scans append-history index pages and row-state pages to construct a live
  entryset for complete exact-index caches.
- `packages/mylite-storage/src/storage.c::scan_exact_index_entries_from()`
  uses the same remove-before-append pattern for exact key scans.
- `packages/mylite-storage/src/storage.c` already has
  `mylite_storage_live_row_id_set`, a hash-backed row-id set used by live-row
  caches.

## Scope

- Track row ids currently represented in an in-progress index entryset with
  `mylite_storage_live_row_id_set`.
- Remove by row id only when that set says the entryset may contain the row.
- Keep row-state replacement/delete handling synchronized with the tracked set.
- Apply the optimization to complete live index entryset scans and exact-key
  append-history scans.
- Preserve focused storage coverage for index-key updates where the old key must
  disappear and the new key must be found through exact index reads.

## Non-Goals

- No new index page format.
- No storage-level B-tree navigation.
- No change to maintained index root publication.
- No change to MariaDB SQL semantics or public `libmylite` API behavior.

## Design

`read_live_index_entries_from()` should keep a local row-id set named for the
entryset under construction. For each relevant index-entry page:

1. If the row id is already in the set, remove existing entryset entries for
   that row and remove the id from the set.
2. Append the current index entry.
3. Add the row id to the set.

For row-state pages:

- `REPLACE` should update the entryset row id only when the source row id is
  tracked. If the replacement id is already tracked, remove it first so the
  replacement does not create duplicate row ids.
- `DELETE` should remove entryset rows and the tracked row id only when the
  source row id is tracked.

`scan_exact_index_entries_from()` should use the same row-id set, with one
difference: any later index-entry page for a tracked row removes the old exact
match first, then appends the row only if the new key still matches the exact
search key.

This preserves the existing replacement semantics while making append-only
history linear in the number of visited index pages.

## Compatibility Impact

No compatibility behavior changes. The affected paths still return the same
row ids for exact-index reads after inserts, updates, deletes, and key changes.

## Single-File And Embedded-Lifecycle Impact

No durable file or lifecycle change. The new row-id set is temporary heap state
while scanning append-history pages.

## Build, Size, And Dependencies

No dependency or binary-profile change. The change reuses existing first-party
storage data structures.

## Test Plan

- Existing storage coverage in `test_active_exact_index_cache_many_replacements`
  proves secondary-index key updates remove the old exact key, publish the new
  exact key, and keep later deletes visible across active and durable reads.
- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev -R 'mylite-storage\.capabilities' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 100000 1000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Exact-index entryset construction avoids unconditional row-id removal for
  never-seen rows.
- Replacement and delete row-state pages keep the temporary row-id set in sync
  with the entryset.
- Existing routed storage tests pass.
- Larger-table prepared update setup no longer samples predominantly in
  `remove_index_entries_by_row_id()`.

## Risks

- The row-id set must be updated in every path that mutates the temporary
  entryset, or exact index scans could retain stale rows. Focused key-change
  coverage plus the existing storage suite guard this.
