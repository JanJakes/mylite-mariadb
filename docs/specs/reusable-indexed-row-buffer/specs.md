# Reusable Indexed Row Buffer

## Problem

The routed update benchmark still spends visible time in exact primary-key row
materialization after inline exact unique cursors. The handler now avoids
per-lookup key, cursor-entry, and offset-array allocations, but
`mylite_storage_find_indexed_row()` still allocates a fresh row payload for each
point lookup and the next cursor build frees the previous payload.

The hot fixed-record update path needs the row payload only long enough to copy
it into MariaDB's record buffer before `update_row()` receives the old row. A
caller-owned reusable row buffer can preserve the current storage API behavior
while avoiding repeated same-size malloc/free churn.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` uses
  `mylite_storage_find_indexed_row()` for non-BLOB durable exact unique
  lookups.
- `read_index_cursor_row()` copies the stored MyLite row payload into the
  MariaDB record buffer and then keeps cursor-owned row payload storage until
  the next cursor clear.
- `packages/mylite-storage/src/storage.c::read_indexed_row_payload_from_open_file()`
  allocates a new row payload whether the payload comes from an active cache,
  durable cache, or decoded row page.
- Existing `mylite_storage_find_indexed_row()` ownership is useful for general
  callers and should remain unchanged.

## Design

- Add `mylite_storage_find_indexed_row_reuse()` beside the existing find helper.
- The new function accepts `unsigned char **inout_row`,
  `size_t *inout_row_capacity`, and `size_t *out_row_size`.
- On success, the storage layer grows the caller buffer only when needed, copies
  the found row into it, and reports the logical row size.
- On not-found or error, the caller buffer and capacity remain owned by the
  caller; output row id and row size are reset.
- Refactor the internal indexed-row materialization helper so existing
  allocation-returning APIs and the new reusable-buffer API share the same row
  lookup, cache, and validation path.
- Teach `ha_mylite` to keep one exact-index row scratch buffer per handler and
  to avoid freeing that scratch from ordinary cursor cleanup.

## Affected Subsystems

- MyLite storage row materialization API.
- MyLite MariaDB storage-engine handler.
- Exact unique primary-key and unique-key point lookup cursors.
- Storage unit tests and storage-smoke update performance baseline.

## Compatibility Impact

No SQL, storage-engine routing, or MySQL/MariaDB behavior change. The existing
allocation-returning storage API remains source-compatible. The new storage API
is a first-party helper for callers that can own reusable row memory.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, transaction, or recovery change.
The reusable buffer is process memory owned by the caller.

## Public API And File-Format Impact

Adds a first-party storage API function. No `libmylite` public C API or file
format change.

## Binary-Size And Dependency Impact

Small first-party C and handler change. No new dependency. Handler instances
gain one reusable row pointer and capacity field.

## Tests And Verification

- Add storage unit coverage that proves `mylite_storage_find_indexed_row_reuse()`
  reuses a sufficiently large caller buffer and preserves ownership on
  not-found.
- Rebuild the MariaDB storage-smoke embedded archive with
  `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`.
- Run a sampled one-million-update benchmark with macOS `sample`.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

Verification after implementation:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC` rebuilt the storage-smoke embedded archive at
  `20.07 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`
- sampled one-million-update benchmark with macOS `sample`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/include/mylite/storage.h
  packages/mylite-storage/tests/storage_test.c
  mariadb/storage/mylite/ha_mylite.cc mariadb/storage/mylite/ha_mylite.h`

Measured update baseline:

- Direct primary-key updates: `11.569 us/op`.
- Prepared primary-key updates: `4.963 us/op`.

Sampled rerun:

- Direct primary-key updates: `11.785 us/op`.
- Prepared primary-key updates: `4.992 us/op`.

The sampled exact lookup frame now uses
`mylite_storage_find_indexed_row_reuse()` and no longer shows a per-row payload
allocation in `read_indexed_row_payload_from_open_file()` after the scratch
buffer is warm. The wall-clock result is neutral to slightly worse for direct
updates, so this slice removes allocator churn but does not yet materially move
the benchmark. Remaining visible work is storage exact-row lookup/cache
maintenance plus MariaDB statement execution overhead.

## Acceptance Criteria

- Existing allocation-returning indexed-row APIs retain their current ownership
  and error behavior.
- The reusable indexed-row API avoids reallocating when the caller buffer is
  already large enough.
- Exact unique durable handler cursors use the reusable row scratch buffer.
- Cursor cleanup never frees handler-owned scratch storage.
- Existing storage and embedded storage-engine tests remain green.
- Update profile evidence shows reduced row-materialization allocator work on
  the primary-key point-update path, even if the wall-clock benchmark remains
  neutral.

## Risks And Open Questions

- This still copies the serialized row payload into MariaDB's row buffer; a
  larger future slice would need row-decoding directly into MariaDB records to
  remove that copy.
- Variable-width non-BLOB rows can still grow the scratch buffer, but fixed-size
  update loops should allocate once and then reuse.
