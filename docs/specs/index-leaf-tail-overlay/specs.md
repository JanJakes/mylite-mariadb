# Index Leaf Tail Overlay

## Problem

Single-level index leaf roots can now answer exact byte-key lookups, but the
first implementation uses a conservative freshness rule: a leaf is used only
when it is the last published page. That makes any later append invalidate all
older leaf roots, even when the later pages are unrelated or can be applied as
an ordered append tail.

MyLite's current storage format is append-only for rows, row-state pages, and
index-entry pages, so a leaf root can remain useful as a point-in-time base
snapshot. The next bounded improvement is to read leaf matches first and then
scan only pages appended after the leaf root for matching new index entries and
row-state tombstones.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` starts equality and
  range reads through `ha_index_read_map()`, while equality continuation uses
  `handler::read_range_next()` and `ha_index_next_same()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()` only
  calls the storage exact lookup APIs for non-nullable full-key byte-exact
  integer-family shapes guarded by `mylite_key_uses_raw_exact_filter()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaf()`
  publishes a leaf root from live index entries. That leaf is a committed
  snapshot at its root page id.
- `packages/mylite-storage/src/storage.c::scan_exact_index_row_ids()` and
  `scan_exact_index_entries()` already apply append-order row-state pages by
  removing source row ids from accumulated exact matches.
- Update publication writes the replacement row, then a row-state page, then
  replacement index-entry pages. Delete publication appends a row-state page.
  Append publication writes row and index-entry pages.

## Design

- Treat an index leaf root as a committed base snapshot when the catalog root
  points at a valid leaf page for the same table id and index number.
- For exact row-id lookup:
  - append matching row ids from the leaf;
  - scan pages after the leaf root;
  - append matching tail index entries;
  - remove row ids hidden by tail row-state pages.
- For exact entryset lookup:
  - append matching entries from the leaf;
  - scan pages after the leaf root;
  - append matching tail entries;
  - remove entries hidden by tail row-state pages.
- Keep the full append-log scan fallback when no root exists.
- Keep the scope restricted to the existing byte-exact storage APIs. General
  ordered reads, prefix reads, nullable-key behavior, and collation-sensitive
  comparisons remain on existing handler cursor paths.
- Do not introduce page splits, free-space management, or maintained B-tree
  mutation in this slice.

## Compatibility Impact

SQL-visible behavior should not change. The handler still uses this path only
for guarded byte-exact full-key equality. The tail overlay applies the same
row-state visibility rules as the current full append-log exact scan.

## Single-File And Lifecycle Impact

No new file types or companions. Leaf pages, row pages, row-state pages, and
index-entry pages remain in the primary `.mylite` file. Older leaf roots can
remain useful as base snapshots until future compaction or root replacement.

## Public API And File-Format Impact

No public API or file-format change. This is a lookup-path improvement over the
existing leaf page and index-root metadata formats.

## Storage-Engine Routing Impact

No routing changes. Durable MyLite tables can use the overlay path through the
existing exact storage APIs. Runtime-volatile MEMORY/HEAP tables keep their
current volatile lookup path.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to refactoring exact scan helpers
to accept a start page and adding leaf-to-row-id overlay helpers.

## Tests And Verification

- Extend storage unit coverage so a leaf root remains usable after later pages:
  - a later append contributes a matching exact secondary-key entry;
  - a later update removes an old leaf entry and contributes a replacement
    entry;
  - a later delete removes an old leaf entry;
  - another index root published later does not force a whole-file fallback for
    earlier roots.
- Keep existing append-only exact scan tests passing.
- Run storage unit tests, storage-engine smoke, compatibility harness
  `storage-engine`, changed-line formatting checks, and `git diff --check`.
- Local verification on this branch:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `tools/mylite-compat-harness run storage-engine`
  - `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `git diff --check`

## Acceptance Criteria

- Exact storage lookup uses leaf roots even when pages were appended after the
  root.
- Tail row-state pages correctly hide row ids present in the leaf snapshot.
- Tail index-entry pages correctly contribute matching appended or replacement
  rows.
- No SQL-visible index behavior changes.

## Risks

- This is still not B-tree navigation. It reduces full-file scans to leaf plus
  append-tail scans for roots that fit in one page.
- Very old roots with long tails can still become expensive until maintained
  multi-page indexes, checkpoints, or compaction exist.
- The optimization remains limited to byte-exact full-key equality.
