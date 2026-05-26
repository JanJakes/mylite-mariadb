# Versioned Packed Row Pages

## Problem

The packed row reader can materialize fixed-size inline pages with
`record_count > 1`, but a production writer cannot safely start a packed page
with its first row. A one-row page with the existing row-page version is
ambiguous: the unmarked physical page id is already the legacy row reference,
while the marked slot-0 id would refer to the same physical payload.

Packed production writes need a page-level discriminator before they can return
marked row references from the first inserted row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` stores handler positions in an
  eight-byte ref buffer and does not have room for an out-of-band page-format
  discriminator.
- `packages/mylite-storage/src/storage_format.h` row pages already include a
  `ROW_PAGE_VERSION` field.
- Legacy MyLite row writers emit row-page version `1` with `record_count == 1`
  and unmarked page-id row references.
- The packed reader added marked row references, but without a page version it
  intentionally refused marked references to legacy one-row pages.

## Design

- Reserve row-page version `2` for fixed-size inline packed row pages.
- Keep row-page version `1` as the legacy one-row format:
  - `record_count == 1`;
  - unmarked physical page id is the only valid row reference.
- For row-page version `2`:
  - `record_count >= 1`;
  - `overflow_root_page == 0`;
  - payload rows are fixed-size and contiguous;
  - marked row references are required, including slot `0`;
  - unmarked page ids do not alias packed rows.
- Update scan/live-row collection to emit marked references for every version-2
  slot, including single-slot packed pages.
- Keep production writers on version `1` in this slice. The test-only packed
  page hook should write version `2`.

## Affected Subsystems

- MyLite row-page metadata decoding.
- Direct row materialization.
- Full scans and live-row id collection.
- Test-only packed page construction.

## Compatibility Impact

No SQL-visible behavior change for existing files. Version-1 row pages keep
their current bytes and row-id semantics.

Version-2 row pages are newly accepted by the reader but are not emitted by
ordinary production writes yet.

## Single-File And Lifecycle Impact

No sidecar or lifecycle change. Version-2 row pages are durable pages inside
the primary `.mylite` file.

## Public API And File-Format Impact

No public API signature change. This reserves row-page version `2` inside the
existing row-page header.

## Storage-Engine Routing Impact

No routing policy change. Routed tables still write legacy pages until the
packed writer lands.

## Binary-Size Impact

Small first-party decoder and test changes. No dependency change.

## Tests And Verification Plan

- Update packed row tests so the test hook writes version-2 pages.
- Cover a single-slot version-2 page:
  - marked slot-0 read succeeds;
  - unmarked physical page-id read returns not found;
  - scans/counts include the marked row id.
- Keep multi-slot packed read and indexed packed read coverage.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Version-1 row pages keep legacy unmarked row ids.
- Version-2 row pages require marked row references for every slot, including
  slot `0` when `record_count == 1`.
- Marked references to version-1 pages and unmarked references to version-2
  pages return not found.
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

- Version-2 currently covers fixed-size inline rows only. BLOB/TEXT and
  variable-sized packed layouts remain separate work.
- Once production writers emit version-2 pages, rollback/recovery and index
  publication need dedicated coverage for first-row packed pages.
