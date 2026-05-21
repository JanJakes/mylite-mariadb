# Exact Unique Index Row Direct Output

## Problem

The durable exact unique-index fast path resolves a key and fetches the row in
one storage operation, but the handler still copies the storage-owned row
scratch into MariaDB's record buffer on the hot `index_read_map()` path.
Prepared primary-key point updates and selects then pay an avoidable copy and
cursor materialization step before the SQL layer can use the row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` calls
  `build_index_cursor()` for exact key reads and then immediately calls
  `read_index_cursor_row()` to copy the first cursor row into `buf`.
- `ha_mylite::build_index_cursor()` already routes durable raw exact unique
  filters for non-BLOB tables through
  `mylite_storage_find_indexed_row_reuse()`, preserving one storage open and
  one storage key lookup, but it stores the payload in `index_row_scratch`.
- `ha_mylite::read_index_cursor_row()` validates the scratch payload size and
  copies it into MariaDB's fixed record buffer.
- `packages/mylite-storage/src/storage.c::copy_row_payload_to_output()` may
  allocate or `realloc()` the output buffer. Passing MariaDB's record buffer to
  that API would be unsafe, so a fixed-output storage API is needed.

## Design

- Add `mylite_storage_find_indexed_row_into()` to the first-party storage API.
  It resolves an exact index key to one row id and copies the row payload into a
  caller-owned fixed buffer in the same storage operation.
- Keep `mylite_storage_find_indexed_row()` and
  `mylite_storage_find_indexed_row_reuse()` behavior unchanged.
- Add an internal storage copy helper that:
  - sets `out_row_size` to the resolved row payload size;
  - copies into the caller buffer when `row_size <= out_row_capacity`;
  - returns `MYLITE_STORAGE_FULL` without writing past the caller buffer when
    the resolved payload is too large.
- Add a handler helper for durable non-BLOB raw exact unique reads. It builds
  the single-entry cursor metadata, validates that the resolved row size equals
  `table->s->reclength`, copies the payload directly into `buf`, clears any
  record BLOB-payload slot, and sets `current_row_id`/`index_row_index`.
- Use the direct-output path only from `index_read_map()` when the exact-key
  cursor must be rebuilt for the active index. Other cursor navigation methods
  keep the existing cursor materialization behavior.

## Compatibility Impact

No SQL behavior changes. The optimization applies only to fixed-record durable
tables with full non-null raw exact unique-key filters. `ENGINE=InnoDB` and
other routed engines still resolve to MyLite before this handler path runs.
BLOB/TEXT tables and volatile rows stay on the existing row-lifetime paths.

## Single-File And Lifecycle Impact

No file-format, sidecar, journal, or lifecycle change. The new API reads the
same durable `.mylite` pages and active statement caches as
`mylite_storage_find_indexed_row_reuse()`.

## Public API And File-Format Impact

The first-party storage package gains one C API function. The public
`libmylite` API and `.mylite` format do not change.

## Storage-Engine Routing Impact

No routing change. The path sits below MyLite engine alias resolution.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to one small storage API
wrapper, one fixed-buffer copy helper, and one handler branch.

## Test And Verification Plan

- Add storage unit coverage for successful fixed-buffer indexed-row lookup,
  missing key, misuse inputs, and too-small destination buffers.
- Run storage unit tests.
- Run storage-engine compatibility smoke.
- Run the local performance baseline for prepared primary-key updates.
- Run `git diff --check` and clang-format diff checks for touched C/C++ files.

## Acceptance Criteria

- `ha_mylite::index_read_map()` can materialize the first durable non-BLOB exact
  unique row directly into MariaDB's record buffer.
- Existing cursor navigation behavior remains valid after the direct first-row
  read.
- Existing storage and storage-engine compatibility tests pass.

## Risks

- This removes a storage-side copy and cursor materialization step; it does not
  address the larger MariaDB prepared-statement range-planning overhead visible
  in samples.
- Returning `MYLITE_STORAGE_FULL` for an undersized fixed buffer is a storage
  API capacity signal. The handler maps a payload larger than `reclength` to
  corruption because that violates the table record contract.
