# Packed Row Read Rowset Batching

## Problem

After maintained-root dirty plan facts, the prepared-insert profile leaves the
remaining maintained-root decodes at protected validation gates:

- `read_index_leaf_run_root`: `1` full-checksum decode;
- `plan_maintained_index_root_inserts`: `2` full-checksum planning decodes;
- `validate_recovery_journal_saved_page`: `2` full-checksum journal decodes.

Those are safety boundaries and are out of scope for this slice. The same
profile still reports `107,078` `row` zero-tail checksum calls through
`decode_row_page_metadata` during verification/readback. Full-row reads first
scan physical row pages to collect live row ids, then materialize each row id
individually. Packed inline row pages therefore decode and checksum the same
physical page once during collection and again once per packed slot during
rowset materialization.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  storage layer.
- `mylite_storage_read_rows()` opens the MyLite file, resolves the table id,
  calls `collect_live_table_row_ids()` when the durable live-row cache is not
  usable, then calls `read_row_ids_into_rowset()`.
- `collect_live_table_row_ids()` scans physical pages, calls
  `decode_row_page_metadata()` once per row page, and records one row reference
  per live slot after row-state compaction.
- `read_row_ids_into_rowset()` checks the durable row-payload cache, then calls
  `read_row_page()` for each cache miss. `read_row_page()` reads the physical
  page and calls `decode_row_page()`, which repeats
  `decode_row_page_metadata()` before `decode_row_page_payload()`.
- `decode_row_page_payload()` can materialize any packed slot from an
  already-decoded row-page metadata view without rechecking the page checksum.
- Existing test hooks can append packed inline row pages and count
  `decode_row_page_metadata` zero-tail checksum calls.

## Design

Add a packed-row page run materializer for `read_row_ids_into_rowset()`:

- after a durable row-payload cache miss, detect packed row references;
- read the referenced physical row page once;
- decode and checksum its row metadata once;
- materialize every consecutive row id in the requested row-id list that points
  at that same physical page using `decode_row_page_payload()`;
- append each row to the rowset builder in the original row-id order; and
- seed the durable row-payload cache for each materialized slot through the
  existing cache helper.

Legacy one-row pages, overflow rows, cache hits, non-packed references, invalid
row references, table-id mismatches, and non-consecutive packed references keep
the existing per-row path or existing error behavior.

The batch helper is deliberately scoped to readback/materialization. It does
not change collection scanning, row-state compaction, row checksum algorithms,
packed-row encoding, insert writer paths, dirty-buffer publication, recovery
journal validation, or maintained-root planning.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, transaction, recovery, or compatibility support status
changes. Rows are returned in the same order with the same row ids and payload
bytes.

## Single-File And Lifecycle Impact

No file lifecycle changes. Durable row pages remain checksummed on disk and are
validated before materialization. The slice does not introduce sidecar files or
change active statement, transaction, rollback, or recovery behavior.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license changes. The binary
size impact is limited to one small static storage helper and a focused
test-hook self-test.

## Tests And Verification Plan

- Add focused storage self-test coverage proving `mylite_storage_read_rows()`
  materializes a three-slot packed inline row page with two
  `decode_row_page_metadata` row zero-tail checksum calls total: one from live
  row-id collection and one from batched materialization.
- Existing packed-row slot, deletion, direct row, indexed row, storage
  recovery, and storage-engine smoke tests cover unchanged behavior.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Full-row reads over packed inline row pages decode each consecutive physical
  packed page once during rowset materialization instead of once per slot.
- Row order, row ids, payload bytes, durable row-payload cache seeding, and
  error behavior remain unchanged.
- Prepared-insert protected maintained-root decode counts stay at `5`, and
  full-page checksum calls stay at the protected validation count.
- Storage and embedded storage-engine smoke verification pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `359.74 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `378.51 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size `33,992,538` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step `78.737 us/op`; full-page checksum calls stayed at
  `8`, protected maintained-root decodes stayed at `5`, total zero-tail
  checksum calls fell from `227,063` to `134,071`, and
  `decode_row_page_metadata` row zero-tail calls fell from `107,078` to
  `14,086`.
- The benchmark kept `21,031` dirty leaf pressure admissions, `66,144` dirty
  leaf merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
  pressure-context builds, and `19,053` planned stores.

## Risks

- The batching helper must not skip table-id checks or hide corrupt packed
  pages; it still decodes and validates the physical page before materializing
  slots.
- Non-consecutive packed row references from the same page are batched only
  when they are adjacent in the requested row-id list. Broader page grouping is
  intentionally deferred to avoid reordering and cache-control churn.
