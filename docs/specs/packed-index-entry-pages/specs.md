# Packed Index Entry Pages

## Problem

Prepared fixed-size inserts now pack many row payloads into one active
row-page version `2`, but append-only index entries still use one durable page
per changed key. The current prepared-insert component benchmark shows this is
the dominant file-growth bottleneck: a 100,000-row run produces tens of
thousands of append-only index-entry pages while row payloads are already
packed.

The append-tail index-entry format needs a packed page shape that keeps
existing v1 pages readable, preserves row-state visibility overlays, and keeps
MariaDB handler row references opaque.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:3929-4086` prepares MariaDB key images,
  calls `mylite_storage_append_row_with_index_entries()`, and stores the
  returned 64-bit row id as the handler row reference.
- `mariadb/storage/mylite/ha_mylite.cc:3628-3699` materializes rows from that
  opaque row reference in `rnd_pos()`.
- `mariadb/sql/handler.cc:7547` and `mariadb/sql/handler.cc:8172` use handler
  row references as opaque bytes for later lookup and comparison.
- `packages/mylite-storage/src/storage.c::write_packed_inline_insert_pages()`
  already owns the hot active fixed-size insert path and currently reserves one
  append page for each changed append-only index entry.
- `packages/mylite-storage/src/storage.c::read_live_index_entries_from()`,
  `read_live_index_entries_from_lower_bound()`, `read_live_index_entrysets()`,
  and the exact/prefix scan helpers walk append-tail index-entry pages in file
  order and apply later row-state visibility.

## Design

- Add page-version `2` for the existing table-index append-tail page type.
  Version `1` remains the legacy one-entry page shape.
- Keep the existing page type and magic so append-tail scanners route both
  versions through the same index-entry page path.
- Encode version `2` as a fixed-key-size run:
  - page id;
  - catalog table id;
  - entry count;
  - used bytes;
  - MariaDB key number;
  - fixed key size;
  - checksum;
  - repeated cells of row id plus key bytes.
- Decode v1 and v2 through a shared run decoder and entry iterator. V1 exposes
  a one-entry run, so existing v1 files remain readable.
- Use packed v2 pages only in the active packed inline insert path for
  append-only index entries:
  - append to a cached v2 page when table id, key number, and key size match
    and capacity remains;
  - reuse a cached v2 page only when later buffered tail pages are row pages,
    so row-state, maintained-root, or other index pages keep append-tail
    visibility ordering;
  - otherwise allocate one new v2 page and seed the cache;
  - preserve the existing row-page packing and maintained-root planning
    boundaries.
- Capture buffered-page undo before appending an entry to a v2 page that
  predates the current statement frame, then mark its checksum dirty. Rollback
  should restore the old entry count and payload prefix through the existing
  append-buffer undo path.

## Non-Goals

- No public API changes.
- No MariaDB handler or SQL semantics changes.
- No conversion of existing v1 pages.
- No update rewrite conversion to v2 pages in this slice.
- No final B-tree navigation replacement for append-tail scans.

## Affected Subsystems

- MyLite append-tail index-entry page format.
- Active fixed-size packed insert writer.
- Append-tail exact, prefix, lower-bound, and rebuild scans.
- Active append-buffer rollback and checksum refresh for packed index pages.

## Compatibility Impact

SQL-visible behavior is unchanged. `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, and default routed durable tables still resolve to MyLite
storage under the embedded profile. The change only reduces internal page
volume for eligible active append-only index entries.

V1 append-only index-entry pages remain readable, and v2 pages store the same
logical `(table id, index number, row reference, key bytes)` entries in append
order.

## Single-File And Embedded Lifecycle Impact

All durable state remains in the primary `.mylite` file. No new companions are
introduced. Active statement and transaction rollback continue to use the
existing append buffer, journal, and buffered-page undo mechanisms.

## Public API And File-Format Impact

No public API signature change. The file format gains table-index page
version `2` for packed append-tail entries while retaining version `1` decode
support.

## Storage-Engine Routing Impact

No routing policy change. Routed durable tables benefit when their fixed-size
active insert path emits append-only fallback entries.

## Binary-Size, License, And Dependency Impact

Small first-party storage changes only. No dependency or license change.

## Test And Verification Plan

- Extend active packed indexed insert coverage so multiple rows with one
  append-only index:
  - share one packed row page;
  - share one packed index-entry page;
  - remain visible through exact lookup before and after commit;
  - remain filterable after delete.
- Add rollback coverage for appending to an existing packed index-entry page
  inside a nested statement.
- Verify existing v1 index-entry page test hooks still read through the shared
  decoder.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- V1 append-only index-entry pages remain readable.
- Active packed fixed-size inserts can pack multiple same-shape append-only
  index entries into one v2 page.
- Exact, prefix, lower-bound, and rebuild scans see all entries from v1 and v2
  pages in append order and preserve row-state visibility.
- Rollback restores packed index-entry page entry counts and payload bytes.
- The prepared-insert component benchmark shows a substantial final page-count
  reduction for the append-only index-entry tail.

## Verification Results

Run on May 27, 2026 in the `custom-storage` worktree:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `./build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  (`mylite-storage.capabilities`, 148.44 seconds).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed (`mylite-storage.capabilities` and
  `libmylite.embedded-storage-engine`, 178.58 seconds).
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step component was 44.692 us/op; final page count
  was 70,990 pages.

## Risks And Unresolved Questions

- This slice keeps append-tail scans; it reduces write volume and file size but
  does not replace scan fallback with final navigable index pages.
- The first writer cache can be intentionally small. If multi-index workloads
  still interleave enough to allocate many v2 pages, a later slice can add a
  bounded per-index cache without changing the v2 page format.
