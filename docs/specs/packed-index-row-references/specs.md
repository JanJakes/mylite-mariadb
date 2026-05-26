# Packed Index Row References

## Problem

Packed row pages are now readable through marked row references, but index
entry decoders still validate stored row ids as physical page ids. A future
packed writer must be able to store the same opaque 64-bit row reference in
append-only index pages, maintained roots, and published leaf pages.

This slice updates the read-side validation boundary before any production
writer emits packed index entries.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` keeps handler row references at eight
  bytes and uses that value for index cursor materialization, `rnd_pos()`,
  update/delete, duplicate-key checks, and FK actions.
- `packages/mylite-storage/src/storage_format.h` stores row ids as 64-bit
  fields in append-only index-entry pages, index leaf cells, maintained root
  cells, and branch child fence cells.
- `packages/mylite-storage/src/storage.c::decode_index_entry_page()` and
  `decode_buffered_index_entry_page()` currently reject marked packed row
  references by checking `is_addressable_page_id(header, row_id)`.
- `decode_maintained_index_root_page()` and `decode_index_leaf_page()` perform
  the same physical-page-id validation for cell row ids.

## Design

- Treat stored index row ids as opaque row references at decoder boundaries.
- Replace physical page-id validation with `is_addressable_row_reference()` for:
  - append-only index-entry pages;
  - buffered append-only index-entry pages;
  - maintained single-page root cells;
  - published index leaf cells.
- Keep key ordering unchanged: row ids remain the tie-breaker for duplicate key
  bytes. Marked packed references sort by their 64-bit encoded value, which
  preserves slot order within one packed page.
- Keep branch fence semantics unchanged in this slice. Branch cells already
  store max row ids copied from leaves; deeper packed branch coverage should
  come with a branch-root writer test.
- Add a storage test hook that appends one append-only index-entry page for an
  arbitrary row reference, then cover exact indexed lookup into a packed row
  slot.

## Affected Subsystems

- Append-only index-entry decoding.
- Maintained root and published leaf page validation.
- Exact index scans and indexed row materialization for packed row references.

## Compatibility Impact

No SQL-visible behavior change for ordinary writers. Existing index entries
store unmarked legacy row ids and keep the same bytes.

The new behavior is internal and fail-closed: a marked row reference in an
index is accepted only when its physical page id is addressable, and row
materialization still verifies the referenced packed slot.

## Single-File And Lifecycle Impact

No sidecar or lifecycle change. Test-only packed index entries are ordinary
index-entry pages inside the primary `.mylite` file.

## Public API And File-Format Impact

No public API signature change. This uses the existing 64-bit row-id fields in
index pages.

## Storage-Engine Routing Impact

No routing policy change. The production handler still writes legacy row pages
and index entries; this prepares the read path for a later packed writer.

## Binary-Size Impact

Small first-party validation changes and one test hook. No dependency change.

## Tests And Verification Plan

- Add storage unit coverage that appends a test-only packed row page and a
  test-only append-only index-entry page pointing to one packed slot.
- Verify exact indexed lookup returns the packed row id and payload.
- Verify stale packed index entries are filtered after deleting the packed row.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Index-entry, maintained-root, and index-leaf decoders validate row references
  rather than assuming raw physical page ids.
- Exact indexed lookup can materialize a packed row slot from an append-only
  index-entry page.
- Row-state deletion hides a packed indexed row without corrupting sibling
  packed slots.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed.

## Risks And Unresolved Questions

- Published branch roots need explicit packed-row coverage when a packed writer
  can publish leaf/branch roots.
- Numeric row-id tie-break ordering is preserved as encoded-value ordering; if
  future compatibility work needs a different logical ordering, it must be
  specified before writing packed index roots broadly.
