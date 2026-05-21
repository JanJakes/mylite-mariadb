# Full Index Leaf-Run Reads

## Problem

MyLite can publish sorted fixed-width index leaf runs during supported rebuild
paths, and exact-key lookups already use those roots before scanning the append
tail. Full index cursor construction still calls
`mylite_storage_read_index_entries()`, which rebuilds the live index by scanning
the whole append history from page 2 even when a published leaf run is
available.

That keeps broad ordered index scans and handler cursor builds tied to old
append-log cost. It also leaves the existing leaf-run publication useful only
for exact probes.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  calls `mylite_storage_read_index_entries()` for unfiltered durable index
  cursors, then sorts and materializes rows at the handler layer.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  currently finds the table id and calls `read_live_index_entries()`, which
  scans every published page and applies row-state pages as it goes.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_entries()`
  already reads catalog-backed index-root metadata, derives the contiguous
  leaf-run length from the root entry count, reads matching leaf pages, and
  overlays append-tail index-entry and row-state pages after the leaf run.
- Existing leaf pages are sorted by raw key bytes and row id, have a fixed key
  size per run, and live in the primary `.mylite` file. They are immutable base
  snapshots; later row mutations remain append-tail overlay records.

## Design

Teach `mylite_storage_read_index_entries()` to use a published leaf run when
one exists:

- read the catalog image once and locate the table entry;
- look for index-root metadata for the requested index;
- if no root exists, keep the current append-history scan;
- if a root exists, append every leaf-run entry into the output entryset in
  leaf order, then scan only the append tail after the run to apply later
  index-entry and row-state records.
- keep SQL leaf-root publication limited to raw fixed-width keys that already
  use the storage exact-key path. Other supported key shapes continue to use
  full append-history scans until their leaf-run encoding is explicitly covered.

The tail overlay uses the same live-index maintenance semantics as the current
full-history scan:

- a replacement row-state updates base leaf row ids when the key image is
  unchanged;
- a later changed-key index entry removes the replaced row id and appends the
  new key image;
- delete/truncate row-states remove matching row ids.

The helper validates every page in the contiguous run and checks cross-page
ordering so malformed published roots are corruption, not silent fallback. The
handler may still sort the returned entries, so SQL result ordering remains
owned by the existing handler code.

## Non-Goals

- No B-tree page splits or maintained navigable index pages.
- No new file-format fields.
- No changes to exact lookup behavior.
- No variable-width, collation-aware string, BLOB/TEXT, or mixed-shape SQL
  leaf-root publication.

## Compatibility Impact

SQL-visible behavior should not change. The same row-state overlay rules define
which index entries are live, and the handler continues to sort cursor entries
before exposing ordered reads.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. The slice only changes read
selection between a catalog-rooted immutable leaf run plus append tail and the
older full append-history scan. It adds no companion files and no recovery
state.

## Binary-Size And Dependency Impact

No new dependency. The code impact should stay inside first-party storage read
helpers and storage unit tests.

## Tests

- Add storage unit coverage where rows are appended in reverse key order, a leaf
  root is rebuilt, and `mylite_storage_read_index_entries()` returns leaf-run
  order rather than append order.
- Verify append-tail row insertion after the root appears in full index reads.
- Verify updates with unchanged and changed key images apply through tail
  overlay.
- Verify deletes remove base leaf entries.
- Keep existing multi-page leaf exact lookup tests green.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
git diff --check
```

## Acceptance Criteria

- `mylite_storage_read_index_entries()` uses a catalog-backed published leaf run
  as the base snapshot when one exists.
- Append-tail row insertion, unchanged-key replacement, changed-key replacement,
  and delete overlays preserve the same live-entry results as the old full scan.
- Missing roots still use the existing full append-history scan.
- SQL DDL does not publish leaf roots for non-raw key shapes, and removes any
  stale matching root when such a DDL key is seen.
- Existing storage and embedded storage-engine tests pass.
