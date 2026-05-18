# Single-Level Index Leaf Pages

## Problem

MyLite now has catalog-backed index-root metadata, but exact index reads still
fall back to scanning append-only index-entry pages. The next step toward
SQLite-like indexed lookup is a real root-page read path. A full B-tree would
combine page layout, split policy, write amplification, free-space ownership,
and transaction-aware maintenance into one large slice, so this slice introduces
a bounded single-level leaf snapshot first.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` starts range reads
  through `ha_index_read_map()`, and `handler::read_range_next()` uses
  `ha_index_next_same()` for equality continuation.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()` uses
  `mylite_storage_find_index_entry()` for guarded unique raw exact reads and
  `mylite_storage_read_exact_index_entries()` for guarded non-unique raw exact
  reads.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_uses_raw_exact_filter()`
  limits byte-exact storage lookup to integer-family full-key shapes. General
  collation-sensitive and nullable comparisons stay in MariaDB key-tuple
  comparison.
- `packages/mylite-storage/src/storage.c` stores append-only index-entry pages
  containing table id, MariaDB key number, row id, and key-tuple bytes. Row
  updates/deletes append row-state pages that hide older row ids.
- `packages/mylite-storage/src/storage.c::mylite_storage_store_index_root()`
  now provides catalog publication for one root page per table/index pair, with
  table rename/drop/schema-drop lifecycle handled by the catalog.

## Design

- Add an index leaf page type under the existing MyLite index-page family. A
  leaf page stores one table id, one MariaDB key number, an entry count, used
  bytes, and sorted `(row id, key bytes)` cells.
- Keep the first format deliberately single-page. If the live entries for an
  index do not fit, publishing the leaf returns `MYLITE_STORAGE_FULL` and
  callers keep the existing append-log scan behavior.
- Add a first-party storage API that rebuilds and publishes one single-level
  leaf snapshot for an index:
  - scan current live append-only index entries,
  - sort by raw key bytes then row id,
  - append the leaf as the next page,
  - update the index-root catalog record to that new page in the same rollback
    journal publication as the header.
- Exact storage lookup can use the leaf when the catalog root points at a valid
  leaf page for the same table id and index number. The later tail-overlay
  slice applies pages appended after the root for row-state and index-entry
  visibility.
- The leaf path is intentionally restricted to byte-exact storage APIs. General
  ordered reads, prefix reads, nullable-key behavior, and collation-sensitive
  comparisons remain on existing handler/materialized cursor paths.

## Compatibility Impact

SQL-visible behavior should not change. The leaf stores the same key images and
row ids already used by the append-only exact lookup path, and the handler guard
for byte-exact equality remains unchanged.

## Single-File And Lifecycle Impact

Leaf pages live in the primary `.mylite` file. Publishing a leaf appends a new
page and repoints the catalog index-root record under the existing rollback
journal. Older leaf pages become unreachable until free-space management exists,
matching the current orphaned-definition and orphaned-row-page model.

## Public API And File-Format Impact

- The first-party storage API gains a root rebuild function for one table/index
  pair.
- The file format gains a single-level index leaf page type while keeping the
  format version unchanged during early development.
- Existing append-only index-entry pages remain the authoritative mutation log
  and scan fallback.

## Storage-Engine Routing Impact

No engine routing changes. Durable MyLite tables can publish leaf snapshots.
Runtime-volatile MEMORY/HEAP tables remain process-local and keep their current
volatile exact lookup path.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to leaf page encode/decode,
single-page sorting, lookup helpers, and unit tests.

## Tests And Verification

- Add storage unit coverage for publishing a leaf root, reading exact unique
  and non-unique matches through the leaf, and missing-key lookup.
- Cover later append visibility by appending a later row after publishing a
  leaf and proving exact lookup sees the append-log state in addition to leaf
  contents.
- Cover leaf publication after table rename through existing index-root
  lifecycle and root metadata reads.
- Keep existing append-only index tests passing.
- Run storage unit tests, storage-engine smoke, compatibility harness group,
  changed-line formatting checks, and `git diff --check`.
- Local verification on this branch:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `tools/mylite-compat-harness run storage-engine`
  - `tools/mylite-perf-baseline`
  - `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `git diff --check`
- The local default performance-baseline run measured direct/prepared
  primary-key point selects at `2575.470` / `2707.690` us/op and
  direct/prepared secondary-index exact selects at `4950.390` / `4477.870`
  us/op. This is noisy workstation evidence and the harness does not yet
  publish leaf roots, so it is a fallback-path sanity check, not a product
  benchmark.

## Acceptance Criteria

- A durable single-page index leaf can be built from live append-only index
  entries and catalog-published as an index root.
- Exact storage lookups use a valid leaf root and fall back to append-log
  scanning when no root exists.
- Later row/index/state pages do not make lookups return stale leaf contents.
- Existing SQL-visible index behavior remains unchanged.

## Risks

- This is not a full B-tree. It is a root/leaf read path and page-format
  foundation for later split and maintenance work.
- The current single-page limit means large or wide indexes continue to use the
  append-log scan path.
- Sorting by raw key bytes is suitable only for the guarded byte-exact storage
  lookup APIs. Handler-visible general ordering still requires MariaDB key
  comparison.
