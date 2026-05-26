# Packed Row References

## Problem

The current durable row format writes one small logical row per 4096-byte row
page. The prepared insert component benchmark now reports a 10k insert run at
`45473792` final bytes and `11102` header pages, which is close to one page per
logical row. That write amplification is the next major blocker for
SQLite-like insert throughput.

The row page header already has a `record_count` field, but the implementation
currently rejects any row page whose record count is not `1`. More importantly,
the persisted row id is treated as the physical row page id across handler
positions, index entries, row-state replacement/delete pages, exact-index
caches, active live-row caches, and direct row reads. Packing multiple rows
into one physical page therefore cannot be added by only changing
`encode_row_page()`: every row reference must first become an explicit storage
row reference rather than an assumed page id.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` stores `current_row_id` in the handler
  `ref` buffer with `ref_length = sizeof(ulonglong)` and retrieves rows by that
  value in `rnd_pos()`, update, delete, duplicate checks, FK cascades, and index
  cursor materialization.
- `packages/mylite-storage/src/storage_format.h` row pages contain
  `ROW_RECORD_COUNT`, but `decode_row_page_metadata()` and
  `decode_buffered_row_page_metadata()` reject `row_count != 1`.
- `packages/mylite-storage/src/storage.c::read_row_page()` reads page
  `row_id` directly and `decode_row_page_metadata()` requires the stored page
  id to equal that page id.
- Index-entry, index-leaf, maintained-root, and branch pages store row ids as
  64-bit values and use them for duplicate-key tie-breaking, cursor
  continuation, exact-cache membership, and row materialization.
- Row-state pages store 64-bit source and replacement row ids. Update/delete
  visibility and rollback depend on comparing those values exactly.

## Design

Introduce an internal row-reference boundary before adding packed rows.

- Keep the public/internal storage API type as `unsigned long long row_id` for
  now. Handler refs and existing files continue to store the same 64-bit value.
- Add first-party helpers that make the current legacy encoding explicit:
  - legacy row reference: `row_id == physical_row_page_id`;
  - slot index: always `0`;
  - physical row page id: `row_id`.
- Route direct row materialization through those helpers so future packed row
  references have one place to decode page id and slot.
- Keep index-entry and row-state byte formats unchanged in this prerequisite
  slice. They still store the 64-bit row reference value.
- Do not enable `row_count > 1` yet. Packed row pages need a follow-up file
  format slice that defines slot layout, row offsets, row-id ordering, and
  compatibility with row-state replacement chains.

The likely packed-row encoding should preserve 64-bit handler refs and index
storage by treating the row id as an opaque row reference. A follow-up format
slice can use a versioned row page type or format flag to distinguish legacy
page-id row refs from packed row refs. The design should avoid changing
MariaDB handler `ref_length` unless there is no viable 64-bit encoding.

## Affected Subsystems

- MyLite storage row materialization and metadata decoding.
- Handler row refs, direct reads, updates, deletes, FK cascades, and index
  cursor materialization as compatibility constraints.
- Future packed row-page format and pager/free-space work.

## Compatibility Impact

No SQL-visible behavior change in the prerequisite helper slice. Existing files
remain legacy one-row pages, existing row ids remain physical page ids, and
`ENGINE=InnoDB` / MyISAM / Aria routed tables continue through the same MyLite
handler.

## Single-File And Lifecycle Impact

No durable file-format change in the prerequisite. The follow-up packed-row
format must keep durable state in the primary `.mylite` file and must use the
existing checkpoint/journal rules or a later documented WAL/pager rule.

## Public API And File-Format Impact

The prerequisite is internal only and preserves existing storage API signatures.
The packed-row format follow-up may need a new row page version or file-format
feature flag, but this slice should not write new durable bytes.

## Binary-Size Impact

Small first-party helper code only. No new dependency.

## Tests And Verification Plan

- Add or keep storage tests proving ordinary append/read/update/delete/index
  paths still pass with legacy row references.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Direct row reads use an explicit row-reference-to-page helper instead of
  treating every row id as a page id at the call boundary.
- Legacy files and all existing row-id equality semantics remain unchanged.
- No row page with `record_count > 1` is accepted or written yet.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`:
  passed and continued to report `45473792` final bytes and `11102` header
  pages.

## Risks And Unresolved Questions

- A no-op helper is only useful if the next packed-row format slice actually
  consumes it. Do not spread helper calls mechanically through every comparison
  until a real packed encoding requires it.
- The packed-row encoding still needs decisions for slot layout, variable-size
  row offsets, row-state replacement chains, free-space reuse, and cursor
  continuation ordering.
- Existing row ids may be persisted in user-visible dump-like workflows through
  handler refs only transiently, but this should be verified before changing
  encoding.
