# Uncached Read Rows Scan Materialization

## Problem

After packed-row rowset batching, the prepared-insert smoke profile reduces
verification/readback `decode_row_page_metadata` calls from `107,078` to
`14,086`, but the uncached full-row read still decodes each packed physical row
page twice:

- `collect_live_table_row_ids()` scans row pages and decodes metadata to build
  the compact live row-id list; then
- `read_row_ids_into_rowset()` materializes those row ids and decodes each
  packed physical page once more.

The remaining maintained-root decodes are protected validation gates and stay
out of scope. This slice targets only redundant row-page readback validation in
uncached full-row reads.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  storage layer.
- `mylite_storage_read_rows()` first tries the durable live-row-id cache. On a
  miss it calls `collect_live_table_row_ids()`, then materializes with
  `read_row_ids_into_rowset()`.
- `scan_table_row_pages()` already builds the row-state map, scans physical row
  pages, validates row-page metadata once, skips hidden row ids, and hands each
  live row payload to a callback.
- `scan_table_row_pages()` is already used by truncate support to enumerate
  live rows; it preserves row-state filtering and row-page corruption handling.
- `append_row_to_rowset_builder()` currently assumes the caller knows the final
  row count. A scan materializer does not know that count before the scan, so
  the rowset builder needs bounded dynamic metadata growth.

## Design

Add an uncached full-row read path for `mylite_storage_read_rows()`:

- when the durable live-row-id cache is not used, initialize a rowset builder
  with growable metadata arrays;
- call `scan_table_row_pages()` with a callback that appends the live row id to
  the live-row-id list, appends the payload to the rowset builder, and seeds
  the durable row-payload cache through the existing helper;
- after a successful scan, store the durable live-row-id cache as before; and
- keep the existing `read_row_ids_into_rowset()` path for durable live-row-id
  cache hits.

The rowset builder metadata growth is conservative: existing exact-capacity
callers keep their preallocated arrays and only grow if a future caller exceeds
capacity. The new scan path starts at zero capacity and grows by powers of two.

Do not change row-page checksum algorithms, row-state visibility semantics,
packed-row encoding, insert writer paths, dirty-buffer publication, journal
validation, maintained-root planning, or durable cache invalidation rules.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, transaction, recovery, or compatibility support status
changes. Full-row reads keep row order, row ids, payload bytes, and active
validated-live-row marking.

## Single-File And Lifecycle Impact

No file lifecycle changes. Durable row pages and row-state pages remain
validated before use. No sidecar files or format changes are introduced.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license changes. The binary
size impact is limited to a small scan callback and rowset-builder growth
helper in first-party storage code.

## Tests And Verification Plan

- Add focused storage self-test coverage proving uncached
  `mylite_storage_read_rows()` over a three-slot packed row page records one
  `decode_row_page_metadata` row zero-tail checksum call total.
- Keep existing packed-row, row deletion, row payload cache, recovery, locking,
  and embedded storage-engine smoke tests.
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

- Uncached full-row reads materialize live rows during the row-page scan,
  avoiding a second row-page metadata decode for packed physical pages.
- Durable live-row-id cache storage, durable row-payload cache seeding, row
  order, row ids, payload bytes, and error behavior remain unchanged.
- Protected maintained-root decode counts stay at `5`, and full-page checksum
  calls stay at the protected validation count.
- Storage and embedded storage-engine smoke verification pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; clang-format did not modify files.
- `cmake --build --preset dev --target mylite_storage_test`: passed; no work
  to do.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed before the
  full CTest run.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `318.60 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed; no work to do.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `327.89 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,993,482` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  was saved to `/tmp/mylite-uncached-read-rows-scan-materialization-benchmark.txt`.
  The sampled prepared insert step was `73.184 us/op`, full-page checksum calls
  stayed at `8`, zero-tail checksum calls dropped to `127,063`,
  `decode_row_page_metadata` row zero-tail calls dropped to `7,078`, and
  maintained-root decodes stayed at `5` across the protected call sites.

## Risks

- `scan_table_row_pages()` builds the row-state map before scanning row pages.
  This preserves visibility but may trade fewer row checksums for a similar
  page-read count on some hosts; benchmark evidence must be recorded.
- Dynamic rowset metadata growth must preserve cleanup behavior on allocation
  failures and must not disturb exact-capacity callers.
