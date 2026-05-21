# Maintained Index Root Insert Maintenance

## Problem

Maintained root pages can now be published and read, but row inserts still
append separate index-entry pages even when a catalog root points at a
maintained single-page root with room for the new key. That keeps correctness,
but it does not reduce append volume or exercise the dirty-page journal path
needed for mutable roots.

This slice should let ordinary row inserts update eligible maintained root
pages in place and skip the duplicate append-only index-entry page for those
keys.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  currently begins a write journal, writes row pages, then calls
  `write_index_entry_pages()` for every supplied index entry.
- `packages/mylite-storage/src/storage.c::write_index_entry_pages()` already
  supports an `index_entry_changed` mask; entries with a false bit are skipped.
- `packages/mylite-storage/src/storage.c::begin_write_journal_for_statement_pages()`
  can protect a bounded set of dirty page ids before writes begin, and falls
  back to the same recovery journal path for non-active operations.
- `packages/mylite-storage/src/storage.c::decode_maintained_index_root_page()`
  validates maintained root page ownership, key size, sorted cells, and
  checksums.

## Scope

- Plan maintained root insert targets before beginning the write journal.
- Protect those root page ids in the recovery journal.
- After writing the row page, insert eligible key/row-id cells into the root
  page in sorted order and refresh its checksum.
- Skip append-only index-entry pages for the maintained entries that were
  written into roots.
- Fall back to append-only index-entry pages when a root is absent, not a
  maintained root, full, key-size mismatched, duplicated in the same insert
  request, or cannot be journal-protected.

## Non-Goals

- No update/delete physical removal from maintained roots.
- No root split, merge, compaction, or multi-page maintained leaf.
- No support for maintaining empty roots without known key width.
- No new public API.

## Design

The append path should read the catalog image once, resolve the table record,
and build a small insert plan over the supplied index entries. Each planned
entry records the index-entry slot and maintained root page id. The plan also
builds an `index_entry_changed` mask initialized to all changed; a planned
maintained root insert clears the matching bit so `write_index_entry_pages()`
does not append a duplicate index-entry page.

Planning accepts a root only when:

- the catalog has an index-root record for the entry's index number;
- the root page decodes as `TABLE_INDEX_ROOT`;
- the root table id and index number match;
- the incoming key size equals the root key size;
- the root has one free cell; and
- the same root page has not already been planned for this insert call.

The write journal must be created with the planned root page ids before row or
root pages are written. After the row payload pages are written and the final
row id is known, each planned root page is re-read, decoded, updated with the
new `(row id, key bytes)` cell, and written through the pager. If any planned
root update becomes invalid at write time, the operation fails rather than
silently writing an unindexed row.

If an active statement already owns an immutable recovery journal that cannot
register the planned root page, the append path clears the maintained-root plan
and falls back to append-only index-entry pages. This keeps the operation
correct and avoids dirtying an unprotected root.

Index-leaf rebuild scans also read maintained root pages as live index sources.
They remove any existing entry for the same row id before appending the root
entry, and later row-state pages continue to hide or retarget stale root rows.
That preserves entries inserted directly into maintained roots when a later
rebuild or rename relies on rebuilt root metadata.

## Compatibility Impact

SQL-visible results should not change. Reads already combine maintained root
entries with later row-state overlays. Updates and deletes can continue to use
row-state pages and append-only replacement index entries; stale root entries
are hidden by the existing overlay logic. Maintained-root metadata reports the
physical root page entry count, so eligible inserts advance the metadata count
while later update/delete overlays do not physically remove stale root cells.

## Single-File And Lifecycle Impact

Root rewrites happen in the primary `.mylite` file and are protected by the
existing recovery journal. No new sidecar files are introduced.

## Test Plan

- Publish maintained roots through small rebuilds.
- Insert a row whose indexed keys fit those roots and verify the file grows by
  only the row page, not extra index-entry pages.
- Verify exact and full index reads include the root-maintained inserted row.
- Verify later update/delete row-state overlays still hide stale root entries.
- Keep fallback coverage for oversized immutable leaf runs through existing
  multi-page leaf tests.
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

- Eligible inserts update maintained root pages and skip duplicate append-only
  index-entry pages.
- Fallback insert behavior remains unchanged for roots that are absent,
  immutable, full, or unsupported.
- Recovery journal setup includes dirty maintained root pages before they are
  rewritten.

## Initial Implementation

- `mylite_storage_append_row_with_index_entries()` reads the catalog image,
  plans eligible maintained root updates, protects root page ids in the write
  journal, rewrites root pages through the pager, and passes an
  `index_entry_changed` mask that skips duplicate append-only index-entry
  pages.
- Maintained root insertion decodes the root, validates table/index/key
  ownership, inserts the new `(row id, key bytes)` cell in sorted order, updates
  used bytes and entry count, and refreshes the checksum.
- Index-leaf rebuild scans now include maintained root pages so entries that no
  longer have append-only index-entry pages remain rebuildable.
- Storage tests assert that eligible root inserts grow the file by only the row
  page, while oversized immutable leaf runs still use row page plus append-tail
  index-entry page growth.

## Verification Results

Passed:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c packages/libmylite/tests/embedded_storage_engine_test.c
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```
