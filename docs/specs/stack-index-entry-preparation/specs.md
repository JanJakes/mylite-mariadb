# Stack Index Entry Preparation

## Problem

The prepared update benchmark spends measurable handler time allocating and
freeing two small buffers for every row update:

- `mylite_prepare_index_entries()` allocates the index-entry array.
- The same function allocates contiguous key storage.

For common routed row DML with a small number of fixed-width keys, those buffers
are tiny and have statement-local lifetime. Heap allocation is unnecessary work
on the hot path toward SQLite-like prepared statement performance.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` prepares key
  entries before duplicate-key checks, FK checks, and storage append.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` prepares key
  entries before changed-key detection, duplicate-key checks, FK checks, and
  storage update.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_prepare_index_entries()` copies
  MariaDB key images with `key_copy()` into MyLite storage-entry descriptors.
- A macOS `sample` run over
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000` showed `mylite_prepare_index_entries()` and allocator frames
  in the hot update path after the current buffered rewrite work.

## Design

Keep `mylite_prepare_index_entries()` as the heap-owning default helper for
call sites that do not provide scratch storage. Add a scratch-aware helper used
by `write_row()` and `update_row()`:

1. Compute the existing key count and total key-storage size exactly as today.
2. Use caller-owned stack entries when the table has at most the small inline
   entry limit.
3. Use caller-owned stack key storage when the total key image bytes fit the
   inline key-storage limit.
4. Fall back to heap allocation for larger or unusual tables.
5. Free only buffers that were not caller-owned scratch.

This changes allocation strategy only. The produced `mylite_storage_index_entry`
descriptors, key bytes, duplicate-key behavior, FK behavior, row payloads,
storage calls, and transaction visibility remain unchanged.

## Compatibility Impact

No SQL-visible behavior changes. MySQL/MariaDB compatibility depends on the
same `key_copy()` images and existing duplicate/FK/storage flows.

## File And API Impact

No file-format, companion-file, public `libmylite`, or storage C API change.
The helper is private to the MyLite MariaDB handler.

## Storage Routing Impact

No routing change. `ENGINE=InnoDB`, `MyISAM`, `Aria`, and omitted/default
engine tables continue to route through the existing MyLite handler and storage
engine paths.

## Binary-Size Impact

Negligible. The slice adds a small private helper and stack buffers in two row
DML methods while removing hot-path allocator calls for common tables.

## Test And Verification Plan

- Build the storage-smoke performance target.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run the prepared update performance baseline before and after the change:
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000`.

## Acceptance Criteria

- Existing routed row-DML, FK, transaction, and WordPress fixture tests pass.
- The prepared-update hot path avoids heap entry/key-storage allocation for the
  benchmark table.
- Larger key counts or key images still fall back to heap allocation.
- The final diff is limited to handler-local allocation strategy plus docs and
  roadmap notes.

## Verification

- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000`: prepared updates measured between `4.358` and `4.554 us/op`
  in post-change local runs.
- macOS `sample` over the prepared-update benchmark showed
  `mylite_prepare_index_entries_with_scratch()` on the hot path without child
  allocator frames for the benchmark table.

## Risks

- Stack scratch must not escape beyond the storage call. The existing handler
  calls consume index entries synchronously, so the scratch lifetime is bounded
  by the row method.
- The free helper must never free caller-owned stack storage.
