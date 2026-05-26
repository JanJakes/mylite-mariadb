# Active Maintained-Root Packed Row Append

## Problem

Active append-only indexed inserts can pack rows when no catalog-backed
maintained index root is involved. Tables with maintained roots still fall back
to legacy one-row pages even though the planner already accepts an opaque row
reference and earlier decoder work accepts marked packed row ids in maintained
root cells.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mylite_storage_append_row_with_index_entries()` predicts the insert row id
  before calling `plan_maintained_index_root_inserts()`.
- `plan_maintained_index_root_inserts()` and maintained-root write helpers pass
  the row id through as a 64-bit value; maintained-root readers now validate it
  as an opaque row reference.
- Direct storage calls without an active append buffer still cannot pack, so
  existing direct maintained-root behavior and page-count assertions remain
  legacy.

## Design

- Allow the insert row-id predictor to choose a marked packed row reference
  even when maintained-root planning is needed, but only when the active append
  buffer can reserve the row page without flushing and every touched maintained
  index root is still an in-place writable root page.
- Pass that predicted marked row id into maintained-root planning.
- Keep the existing legacy fallback when there is no active append buffer,
  the row is oversized, the packed page is full, the append buffer would need
  to flush before reserving the row page, or a touched maintained index has
  already promoted to a branch root or overflow-tail shape.

## Affected Subsystems

- Active fixed-size insert row-id prediction.
- Maintained root insert planning and root write publication.
- Exact indexed reads through maintained roots.

## Compatibility Impact

No SQL-visible behavior change is intended. Maintained roots continue to expose
the same logical index entries, but active fixed-size inserts can store marked
packed row references in root cells.

## Single-File And Lifecycle Impact

No new sidecars or lifecycle states. Packed row pages and maintained root
rewrites remain in the primary `.mylite` file under existing checkpoint and
journal rules.

## Public API And File-Format Impact

No public API signature change. Maintained root cells can now contain marked
packed row references produced by the active insert path.

## Storage-Engine Routing Impact

No routing policy change. Routed durable indexed tables with maintained roots
can use packed active inserts when the row shape fits.

## Binary-Size Impact

Small first-party storage changes only. No dependency change.

## Tests And Verification Plan

- Add storage coverage that initializes an empty maintained index root, inserts
  two fixed-size indexed rows inside one active statement, and verifies:
  - returned row ids are marked packed references on one physical page;
  - the maintained root entry count advances;
  - exact indexed lookup materializes both packed rows before and after commit;
  - the committed file grows by one row page for both rows.
- Cover transaction rollback and rollback-to-savepoint after child statement
  commit so packed maintained-root row slots do not remain visible to row scans
  after the corresponding index entries are undone.
- Update active maintained-root transaction coverage so repeated fixed-size
  inserts into an in-place root assert packed row references and a single
  physical row page.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c packages/libmylite/tests/embedded_storage_engine_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Active maintained-root fixed-size inserts can share one version-`2` packed
  row page while maintained roots remain in-place writable.
- Maintained root cells store marked packed row references and exact lookup
  materializes them.
- Branch-root, overflow-tail, oversized, flush-required, and non-active direct
  inserts stay legacy.
- Direct non-active maintained-root inserts stay legacy.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c packages/libmylite/tests/embedded_storage_engine_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed.

## Risks And Unresolved Questions

- Branch-root split, promotion, and deep branch packed-writer coverage remains
  separate.
- BLOB/TEXT and variable-size packed layouts remain out of scope.
