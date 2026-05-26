# Packed Row Reference Encoding

## Problem

The previous packed-row prerequisite made direct row materialization call
row-reference helpers, but those helpers still defined only the legacy identity
mapping. The next packed-row file-format work needs a concrete 64-bit encoding
that can preserve MariaDB handler `ref_length == sizeof(ulonglong)` and the
existing index-entry and row-state 64-bit row-id fields.

At the same time, MyLite must not accidentally accept future packed row
references before packed row pages exist. A marked row reference should be
recognized at the boundary and rejected as unsupported/not found rather than
being treated as a huge legacy page id or silently mapped to the wrong page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` keeps handler references at eight
  bytes and stores the current storage row id directly in the handler `ref`
  buffer.
- `packages/mylite-storage/src/storage.c::read_row_payload()` and
  `mylite_storage_read_indexed_rows()` validate row ids against header
  addressability before reading row pages.
- `packages/mylite-storage/src/storage.c::read_row_page()` is now the narrow
  direct materialization boundary that decodes a row reference into a physical
  page id and slot.
- Current row pages accept only `record_count == 1`, so any non-legacy row
  reference remains unsupported until the packed row-page layout is introduced.

## Design

- Reserve the top bit of the 64-bit row reference as a packed-reference marker.
- Reserve 12 low bits for a packed row slot. That leaves 51 bits for the
  physical row page id in marked references, which is far beyond practical
  `.mylite` file sizes at the current 4096-byte page size.
- Keep unmarked references as legacy page-id row ids.
- Add helpers for:
  - checking whether a row reference is packed;
  - extracting the physical page id;
  - extracting the slot;
  - testing whether a row reference is addressable by the current legacy reader;
  - constructing marked references for test coverage and future packed-row
    writers.
- Reject marked references in direct row materialization until the packed
  row-page layout exists.
- Update direct row payload and indexed-row materialization addressability
  checks to use row-reference validation instead of raw page-id validation.

## Affected Subsystems

- MyLite storage direct row materialization.
- Handler row-reference compatibility constraints.
- Future packed row-page writer/reader work.

## Compatibility Impact

No SQL-visible behavior change. Existing row ids are unmarked legacy page ids,
and existing `.mylite` files keep their current bytes and semantics.

Marked packed references are deliberately not accepted yet. If an internal test
or future caller supplies one before packed pages are implemented, row reads
return not found instead of reading a wrong page.

## Single-File And Lifecycle Impact

No durable file-format change in this slice. The encoding is reserved in code
and tests, but no writer emits marked references.

## Public API And File-Format Impact

No public `libmylite` API change. The first-party storage API still accepts and
returns `unsigned long long` row ids. Future packed-row file-format work may
write marked references into existing 64-bit row-id fields.

## Binary-Size Impact

Small first-party helper code and one storage test. No new dependency.

## Tests And Verification Plan

- Add a storage regression that appends a legacy row, constructs marked packed
  row references for the same physical page, and verifies ordinary and indexed
  row reads reject those marked references.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Unmarked row ids keep legacy page-id behavior.
- Marked packed row references decode to page id and slot through helpers but
  remain unreadable until packed row pages are implemented.
- Direct row reads and indexed-row reads reject marked references without
  corrupting existing files or weakening legacy row validation.
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

- The 51-bit page-id / 12-bit slot split is a reserved design choice. If packed
  pages later need more than 4096 slots per physical page, the format should be
  revisited before any marked references are written.
- Future code must define how packed row slots are ordered, hidden, replaced,
  and reused before enabling `record_count > 1`.
