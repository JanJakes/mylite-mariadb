# Maintained Index Root Delete Maintenance

## Problem

Maintained root pages can now absorb eligible inserts, but deletes still rely
only on append-only row-state overlays. SQL-visible reads stay correct, but the
root keeps stale `(row id, key bytes)` cells, metadata counts drift upward until
the next rebuild, and future maintained-root capacity is consumed by deleted
rows.

This slice should physically remove deleted row ids from eligible maintained
single-page roots while preserving the existing row-state delete overlay.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::delete_row()` validates the
  current handler row id and calls `mylite_storage_delete_row()` for durable
  MyLite tables.
- `packages/mylite-storage/src/storage.c::mylite_storage_delete_row()` resolves
  the table id, validates the source row id as live, begins a write journal,
  and appends a row-state delete page.
- `packages/mylite-storage/src/storage.c::begin_write_journal_for_statement_pages()`
  can protect a bounded list of existing dirty pages before the first write.
- `packages/mylite-storage/src/storage.c::decode_maintained_index_root_page()`
  validates the root page and exposes sorted fixed-width cells containing row
  ids.

## Scope

- Read the catalog image in the delete path and find maintained root records
  for the target table.
- Plan root page ids whose maintained root page contains the deleted row id.
- Protect planned root page ids in the write journal before appending the
  row-state delete page.
- Rewrite each planned maintained root in place by removing cells for the
  deleted row id, compacting the payload, updating entry count and used bytes,
  and refreshing the checksum.
- Keep the row-state delete page as the visibility authority for append-only
  pages, immutable leaf runs, and fallback cases.

## Non-Goals

- No update maintenance in this slice.
- No root page deletion, free-list reuse, split, merge, or multi-page B-tree.
- No catalog publication solely for root entry-count changes.
- No public API or file-format change.

## Design

The delete path should scan catalog records for index-root metadata belonging
to the target schema, table, and table id. Each root is eligible when:

- the catalog root page id is addressable;
- the page decodes as a maintained `TABLE_INDEX_ROOT`;
- the page table id matches the target table id;
- the page contains the source row id; and
- the bounded dirty-page journal can protect the root page.

If no maintained roots contain the row id, delete behavior is unchanged. If
root planning finds more roots than the bounded journal can protect, or if an
active statement already owns an incompatible immutable journal, the delete
falls back to row-state-only behavior.

The root rewrite removes every cell with the deleted row id, shifts later cells
down, updates entry count and used bytes, zeroes the unused payload tail, and
recomputes the maintained-root checksum. The row-state delete page is still
appended after root rewrites so existing append-only and immutable leaf-run
overlays remain correct.

## Compatibility Impact

SQL-visible delete behavior should not change. Full and exact index reads
already apply row-state overlays, and the root rewrite only removes an entry
that the delete overlay would hide. Maintained-root metadata should now reflect
physical root deletion for eligible single-page roots.

## Single-File And Lifecycle Impact

Root rewrites and the row-state delete page live in the primary `.mylite` file.
The existing recovery journal protects dirty root pages before they are
rewritten. No new companion files are introduced.

## Test Plan

- Storage unit coverage:
  - publish maintained primary and secondary roots;
  - delete a row contained in both roots;
  - verify file growth is only the row-state page;
  - verify exact reads no longer return the deleted row;
  - verify maintained-root metadata counts decrement;
  - verify oversized immutable leaf-run roots still fall back to row-state-only
    behavior.
- Embedded storage-engine smoke:
  - extend the copy-ALTER leaf-root publication case so SQL delete over
    maintained roots decrements root metadata while query results stay correct.
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

- Eligible deletes remove the source row id from maintained root pages in
  place.
- Row-state delete publication remains in place for visibility and fallback
  correctness.
- Deletes that cannot safely protect root pages retain existing behavior.
- Metadata, architecture docs, and roadmap text distinguish delete maintenance
  from the still-planned update maintenance and B-tree work.

## Initial Implementation

- `mylite_storage_delete_row()` now reads the catalog image, resolves the table
  record, and plans maintained root pages that contain the source row id before
  beginning the write journal.
- The delete path protects planned root pages through
  `begin_write_journal_for_statement_pages()`. If the active journal cannot
  accept the dirty-page set, it clears the plan and uses the existing
  row-state-only fallback.
- Root rewrites compact all cells except the deleted row id, zero the unused
  payload tail, update entry count and used bytes, and refresh the checksum.
- Storage and embedded tests assert that eligible maintained-root deletes
  decrement root metadata while file growth remains limited to the row-state
  page.

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

## Risks And Open Questions

- Root rewrites still depend on the bounded dirty-page journal. Tables with more
  maintained roots than the journal limit fall back until journal planning grows.
- Update maintenance needs a separate design because replacement row ids and
  changed-key masks must be coordinated with root removal/reinsertion and
  append-entry skipping.
