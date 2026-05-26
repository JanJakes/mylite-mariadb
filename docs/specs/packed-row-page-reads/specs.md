# Packed Row Page Reads

## Problem

MyLite now has an explicit marked 64-bit row-reference encoding, but marked
references still cannot materialize rows. The next storage step should make the
reader understand the simplest packed row-page shape before any production
writer emits packed pages.

This keeps the change bounded: existing row writers keep producing one-row
legacy pages, while direct reads, table scans, and liveness checks learn how a
future packed writer will expose fixed-size inline rows.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` keeps `ref_length =
  sizeof(ulonglong)`, stores `current_row_id` in `position()`, and reuses that
  value for `rnd_pos()`, `update_row()`, `delete_row()`, direct updates, index
  cursor materialization, and foreign-key cascades.
- `packages/mylite-storage/src/storage_format.h` already reserves row-page
  `ROW_RECORD_SIZE` and `ROW_RECORD_COUNT` fields.
- `packages/mylite-storage/src/storage.c::decode_row_page_metadata()` and
  `decode_buffered_row_page_metadata()` currently reject any row page whose
  `record_count` is not `1`.
- Direct row reads use `read_row_page()`, while full scans and counts use
  `scan_table_row_pages()` and `collect_live_table_row_ids()`.
- Row-state pages compare opaque 64-bit row ids exactly, so packed references
  can remain in the same source/replacement fields.

## Design

- Define the first packed row-page read layout as fixed-size inline records:
  - existing row-page type and version;
  - `record_count > 1`;
  - `overflow_root_page == 0`;
  - payload records stored contiguously at `ROW_PAYLOAD_OFFSET`;
  - row reference is the marked 64-bit value with physical page id plus slot.
- Keep unmarked row references valid only for legacy one-row pages.
- Keep marked row references valid only for packed pages with `record_count >
  1`; a marked reference to a legacy one-row page returns not found instead of
  aliasing the legacy row.
- Teach table scans and live-row collection to emit one logical row per packed
  slot, using marked row references.
- Keep production row writers unchanged in this slice. Add a storage test hook
  to append packed fixed-size row pages for coverage.
- Avoid active buffered in-place rewrite assumptions for packed row ids. Packed
  row updates can fall back to append replacement rows until a later writer and
  rewrite design exists.

## Affected Subsystems

- MyLite storage row-page metadata decoding.
- Direct row materialization and liveness checks.
- Full table scans, counts, and row-id list materialization.
- Test-only storage hooks for future file-format coverage.

## Compatibility Impact

No SQL-visible behavior change for existing files or ordinary writers. Existing
row ids remain unmarked physical page ids, and legacy one-row pages keep their
current bytes.

The new behavior is internal: marked packed row references can read rows only
from pages that explicitly carry `record_count > 1`.

## Single-File And Lifecycle Impact

Packed fixed-size rows are represented inside the primary `.mylite` file. This
slice adds no durable sidecars, journals, WAL files, locks, or temporary
companions.

## Public API And File-Format Impact

No public API signature changes. The storage API continues to use
`unsigned long long` row ids.

The reader now accepts a row-page shape that existing writers do not emit yet:
fixed-size inline row pages with `record_count > 1`. This uses existing row-page
header fields and the previously reserved marked row-reference encoding.

## Storage-Engine Routing Impact

No handler routing policy changes. Routed `ENGINE=InnoDB`, MyISAM, Aria, and
default MyLite tables still write legacy pages until the production packed
writer slice lands.

## Binary-Size Impact

Small first-party helper code and storage unit coverage. No dependency change.

## Tests And Verification Plan

- Add storage unit coverage that appends a test-only packed page and verifies:
  - full reads and counts see every slot;
  - direct marked row reads return the correct slot payload;
  - unmarked page-id reads do not alias the packed slot zero row;
  - deleting one marked row hides only that slot.
- Keep the existing marked-reference-to-legacy-page rejection test.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Legacy one-row pages continue to require unmarked row references.
- Packed fixed-size row pages require marked row references.
- Table scans and row counts include every live packed slot once.
- Row-state delete visibility can hide one packed slot without hiding sibling
  slots on the same physical page.
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

- This slice supports only fixed-size inline packed pages. Variable-size packed
  rows and overflow/BLOB ownership need a separate layout.
- Production writes still use one row page per row. The writer, index-entry,
  update, rollback, and recovery paths must be designed before claiming packed
  insert performance.
- Cursor continuation and ordered index behavior should keep treating row ids
  as opaque 64-bit values; any future ordering assumptions need explicit tests.
