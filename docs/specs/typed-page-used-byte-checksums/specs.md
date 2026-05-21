# Typed Page Used-Byte Checksums

## Problem

Prepared point-update samples still show `checksum_page()` as a top first-party
cost. The hot path repeatedly reads small row, row-state, and append-only
index-entry pages whose logical payloads are much smaller than the fixed
16 KiB page size.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- MyLite encoders for row pages, row-state pages, and append-only index-entry
  pages write checksums with `checksum_page_zero_tail()`, using the typed
  page's used byte count and treating the unused tail as logical zeroes.
- Durable decoders for the same page types still recompute full-page checksums
  with `checksum_page()`.
- Active buffered decoders already skip checksum validation because dirty
  buffered pages are checksummed at flush boundaries.

## Design

- Validate durable row pages with `checksum_page_zero_tail()` after row size,
  row count, and overflow metadata have been validated enough to derive the
  used byte count.
- Validate durable row-state pages with the fixed row-state used byte count.
- Validate durable append-only index-entry pages with
  `INDEX_KEY_OFFSET + key_size` after key-size validation.
- Keep checksum fields, checksum algorithm ids, magic checks, page-id checks,
  table-id checks, and corruption results unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. Decoders now validate the same logical checksum shape that
encoders already write for these typed pages.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only reduces CPU spent validating small typed pages read from the
single `.mylite` file.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Durable row, row-state, and append-only index-entry decoders validate
  checksum values with the same used-byte plus zero-tail shape used by their
  encoders.
- Existing corruption tests for used row payload, row-state, and index key
  bytes still reject corrupted files.
- Existing storage and embedded routed-storage tests pass.

## Risks

- The unused fixed-page tail is no longer treated as durable semantic payload
  for these page types during checksum validation. This matches the existing
  encoder contract, but it means corruption exclusively in unused tail bytes is
  ignored for these small typed pages.
