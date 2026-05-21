# Maintained Index Root Overflow Refill

## Problem

Maintained roots now mark overflow-tail history when a full root falls back to
append-only index-entry pages. That preserves correctness, but the overflow flag
can remain set after later deletes create enough room for all live entries to fit
back in the single-page root. Those roots keep scanning the append tail even
though a compact maintained-root snapshot could represent the complete live
index again.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB handler delete calls still enter MyLite through
  `handler::delete_row()` / `ha_mylite::delete_row()`; this slice changes only
  first-party storage maintenance after a root-resident delete.
- `write_maintained_index_root_deletes()` already rewrites protected maintained
  root pages through the pager before appending the row-state delete page.
- Maintained-root mutations now reject unsafe fallback plans when dirty root
  pages cannot be journal-protected.
- `read_live_index_entries_from()` can overlay append-tail index-entry and
  row-state pages onto a supplied base entryset.
- `encode_maintained_index_root_page_from_entryset()` writes a clean
  single-page maintained root with only the `SINGLE_PAGE` flag when all entries
  fit.

## Scope

- After deleting a row from an overflow-marked maintained root, rebuild the live
  root-plus-tail entryset in memory.
- If the full live entryset fits in one maintained root, rewrite the root page
  from that entryset and clear the overflow flag.
- Leave roots unchanged when live entries still exceed root capacity.

## Non-Goals

- No partial refill when entries still exceed root capacity.
- No root split, merge, free-list reclamation, or tail truncation.
- No catalog format change or public API change.

## Design

The delete path already has the root page protected and dirty. After removing
the deleted row from the in-memory root page, decode it as a maintained root,
append its entries to a temporary entryset, then scan the append tail after the
root page into the same entryset. The scan applies the existing append-only
row-state overlay, so the entryset represents live root-plus-tail entries at the
current header snapshot.

If `entryset.entry_count <= maintained_index_root_entry_capacity(key_size)`,
re-encode the root page with `encode_maintained_index_root_page_from_entryset()`.
That produces a clean single-page root and clears `HAS_OVERFLOW_TAIL`. The
soon-to-be-appended row-state delete page does not need to participate because
the deleted root row has already been removed from the root page before refill.

When the entryset still exceeds capacity, keep the existing root page and
overflow flag. Future deletes can try again.

Once a root has been refilled, later in-place updates and deletes preserve the
current flags. A full root without `HAS_OVERFLOW_TAIL` is still a complete
single-page root and must not re-enable stale tail scans merely because it is at
capacity.

## Compatibility Impact

SQL-visible behavior remains unchanged. This narrows internal tail scans after
delete-heavy workloads reduce an overflowed root back into the single-page
capacity envelope.

## Single-File And Lifecycle Impact

The root rewrite happens in the primary `.mylite` file through the existing
pager and rollback-journal protection for maintained-root deletes. No new
sidecar files are introduced.

## Public API And File-Format Impact

No public API or page-layout change. The existing overflow flag can be cleared
when the root is compact again.

## Storage-Routing Impact

Durable MyLite-routed tables can regain clean maintained-root snapshots after
bounded overflow/delete patterns. Volatile engines are unaffected.

## Binary-Size, License, And Dependency Impact

No new dependency or imported code. Binary impact is limited to a small
first-party storage maintenance helper.

## Test Plan

- Extend overflow-tail storage coverage with a root capacity greater than two:
  overflow the root, delete enough root-resident entries for all live entries to
  fit again, and assert the overflow flag is cleared.
- Verify exact lookup, indexed-row lookup, and full index reads still return the
  live root and formerly-overflowed entries after refill.
- Update a formerly-overflowed entry after refill while the root is full, then
  verify the old overflow-tail key is not visible.
- Keep overflow-row update/delete and maintained-root rollback/recovery tests
  passing.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- An overflow-marked root clears the overflow flag when all live root-plus-tail
  entries fit in one maintained root again.
- Tail visibility remains correct before and after refill.
- Full refilled roots do not re-enable overflow-tail scans during later in-place
  updates.
- Existing maintained-root rollback, recovery, and overflow-tail coverage
  remains green.

## Risks And Open Questions

- Refill is still a local compaction step. It does not reclaim tail pages or
  replace the planned multi-page navigable index.
- Delete-heavy workloads that still exceed single-page capacity continue to use
  the overflow tail until real split/merge support exists.
