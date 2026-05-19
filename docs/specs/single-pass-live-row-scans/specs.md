# Single-Pass Live Row Scans

## Problem

After row-state map buckets, the update-tail scan benchmark improved by
removing per-candidate linear visibility lookup, but the sampled hot path still
showed `scan_table_row_pages()` reading the primary file twice:

1. `build_row_state_map()` scans all pages to collect hidden row ids.
2. `scan_table_row_pages()` scans all pages again to decode row pages and
   filter hidden row ids.

The local 2026-05-19 sample for the ordered full scan after 100k updates showed
time split between `build_row_state_map()`, `read_row_state_page()`,
`read_row_page()`, `read_page_at()`, and page checksums. That is still not
SQLite-like scan behavior for update-heavy files.

## Source References

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage paths:
  - `mylite_storage_read_rows()`
  - `mylite_storage_count_rows()`
  - `scan_table_row_pages()`
  - `read_row_page()`
  - `decode_row_state_page()`

## Design

Keep the existing durable row and row-state page format.

- Add a row-page metadata decoder that validates row-page headers and checksums
  without reading overflow payload blob pages.
- Add a single-pass live-row-id collector:
  - scan each durable page once;
  - append row ids for row pages belonging to the target table;
  - record row-state pages in the hashed row-state map;
  - compact the collected row-id list after the pass using the row-state map.
- Materialize only the compacted live row ids into the output rowset.
- Keep `scan_table_row_pages()` available for existing generic callback paths
  until those callers are migrated deliberately.

## Compatibility

SQL-visible row order remains append-order over live row pages. Hidden source
row ids are still filtered by row-state pages, and corrupt page types still
return corruption errors.

## File Lifecycle

No file-format or companion-file change is introduced. The new row-id list and
row-page metadata are transient heap/stack state inside one storage read.

## Verification

- Existing storage row lifecycle and many-row-state scan coverage must pass.
- Storage-engine smoke must pass.
- Run the local performance baseline and compare the ordered full scan after
  many updates against the row-state bucket baseline.

Local perf sample after implementation:

- row-state bucket baseline ordered full scan after 100k updates:
  `4146.365 us/op`
- single-pass live-row scan ordered full scan after 100k updates:
  `3134.153 us/op`

## Acceptance Criteria

- `mylite_storage_read_rows()` no longer performs a full row-state pre-pass plus
  a second full row-page scan.
- `mylite_storage_count_rows()` uses the same live-row-id collector instead of
  callback scanning.
- The ordered full scan after many updates records a material improvement.
