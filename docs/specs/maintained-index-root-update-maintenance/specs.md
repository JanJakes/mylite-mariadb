# Maintained Index Root Update Maintenance

## Problem

Maintained root pages now absorb eligible inserts and physically remove deleted
row ids, but updates still publish replacement index entries for changed keys
and rely on row-state overlays for unchanged keys. The row-state overlay keeps
reads correct, yet maintained roots continue to carry stale source row ids
until a later rebuild. This wastes root capacity and keeps update-heavy indexed
workloads on the append-tail path.

This slice should physically replace source row cells in eligible maintained
single-page roots during row updates, and skip duplicate append-only
replacement index-entry pages for roots that were updated in place.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` serializes
  new key images, computes `index_entry_changed`, and calls
  `mylite_storage_update_row_with_index_entry_changes()` for durable MyLite
  tables.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  validates the source row id, begins a write journal, then chooses among an
  active buffered rewrite, inline append fast path, or ordinary append of row
  payload, row-state page, and changed index-entry pages.
- `packages/mylite-storage/src/storage.c::write_index_entry_pages()` already
  honors an `index_entry_changed` mask, so planned maintained-root updates can
  clear changed bits to avoid duplicate append-only index-entry pages.
- Maintained root delete and insert helpers already provide sorted root cell
  compaction, insertion, checksum refresh, and dirty-page journal protection.

## Scope

- Plan maintained root update targets before beginning the write journal.
- Protect planned root page ids in the recovery journal.
- For each planned root, remove the source row id and insert the replacement
  `(new row id, new key bytes)` cell in sorted order, including unchanged-key
  indexes whose physical row id still changes.
- Skip append-only index-entry pages for changed entries whose maintained root
  was updated in place.
- Keep the row-state replace page as the visibility authority for immutable
  roots, append-only history, and fallback cases.

## Non-Goals

- No active append-buffer rewrite shape changes.
- No root split, merge, compaction beyond a single page, or free-list reuse.
- No catalog publication solely for maintained-root count-preserving updates.
- No public API or file-format change.

## Design

The update path should read the catalog image once and build a per-index root
update plan over all supplied new index entries, not just changed-key entries.
A root is eligible when:

- the catalog has an index-root record for the entry's index number;
- the root page decodes as a maintained `TABLE_INDEX_ROOT`;
- the root table id, index number, and key size match;
- the root contains the source row id; and
- the same root page has not already been planned for the update call.

The root update removes the source row id first, then reinserts the replacement
row id with the new key bytes. Because the removal happens before insertion,
the page does not need spare capacity for count-preserving updates. If the
active statement already owns an incompatible immutable journal, the plan is
cleared and the existing append-only update behavior is used.

The first implementation should bypass active append-buffer rewrite and inline
append fast paths whenever a root update plan is active. That keeps buffered
statement shape assumptions unchanged. The ordinary append path writes the new
row page and row-state page, rewrites maintained roots through the pager, then
appends only still-needed index-entry pages using the plan's write mask.

Maintained root pages are sorted by raw key bytes and row id. Count-preserving
updates that keep an unchanged duplicate key can therefore reorder exact-entry
results into root order, matching the leaf-run invariant rather than the older
append-tail overlay order.

## Compatibility Impact

SQL-visible update behavior should not change. Reads already use row-state
overlays, and the root rewrite only replaces the root cell that the overlay
would otherwise retarget or hide. Maintained-root metadata counts should remain
stable for count-preserving updates.

## Single-File And Lifecycle Impact

Root rewrites, replacement row pages, and row-state pages live in the primary
`.mylite` file. The recovery journal protects dirty root pages before they are
rewritten. No new companion files are introduced.

## Test Plan

- Storage unit coverage:
  - publish maintained roots;
  - update a row with changed primary and secondary keys;
  - verify file growth omits duplicate append-only index-entry pages for
    maintained roots;
  - verify exact reads find the new keys and not the old keys;
  - verify maintained-root metadata counts remain stable;
  - keep immutable multi-page leaf update behavior unchanged.
- Embedded storage-engine smoke:
  - extend copy-ALTER leaf-root publication coverage to assert update metadata
    count stability and query correctness with maintained roots.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c packages/libmylite/tests/embedded_storage_engine_test.c
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

## Acceptance Criteria

- Eligible updates rewrite maintained roots in place and skip duplicate
  append-only index-entry pages for those planned entries.
- Fallback update behavior remains unchanged when roots are absent, immutable,
  mismatched, duplicated, or cannot be journal-protected.
- Active buffered rewrite and inline update shortcuts remain behaviorally safe.
- Docs and roadmap distinguish this single-page maintained-root work from
  future root splits and transaction-aware maintained index mutation.

## Initial Implementation

- `update_row_with_index_entries()` now reads the catalog, plans eligible
  maintained root updates for all provided index entries, protects those root
  pages in the recovery journal, and falls back to append-only behavior when the
  current active statement cannot protect the planned dirty pages.
- Planned root updates bypass the existing active append-buffer rewrite and
  inline update shortcuts. The ordinary append path writes the replacement row
  and row-state pages, rewrites maintained roots through the pager, and appends
  only still-needed index-entry pages.
- Maintained roots are treated as authoritative live index sources during
  rebuild scans and prefix scans, so stale append-only pages before a maintained
  root do not survive later rebuilds or prefix checks.
- Storage coverage verifies stable root counts, omitted duplicate index-entry
  pages, exact lookups for changed keys, unchanged-key replacement row ids, and
  sorted root-backed exact-entry order. Embedded smoke coverage verifies SQL
  update metadata count stability.

## Verification Results

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c packages/libmylite/tests/embedded_storage_engine_test.c
```

All commands pass on this branch.

## Risks And Open Questions

- Bypassing append-buffer rewrite and inline update shortcuts for planned root
  updates is intentionally conservative. Later slices can reintroduce those
  fast paths once buffered page shape metadata accounts for maintained roots.
- Multi-page maintained indexes still need real B-tree split/merge behavior.
