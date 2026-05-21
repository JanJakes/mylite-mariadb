# Maintained Index Root Publication

## Problem

MyLite now has a maintained index root page format, but no production path
publishes that page type or reads catalog roots that point at it. Published
index roots still assume immutable leaf-run pages whose entry count is derived
from catalog metadata. That keeps exact lookups fast after copy-rebuild DDL,
but it does not move the storage layer toward mutable root-owned index state.

This slice should publish a maintained single-page root when a rebuilt
fixed-width exact index fits in that root, and teach the existing full/exact
index readers to dispatch by the root page type.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_rebuild_index_leaf_roots()`
  selects supported raw exact keys after table-copy DDL and calls
  `mylite_storage_rebuild_index_leaves()`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_uses_raw_exact_filter()`
  restricts storage-level exact lookup to guarded full-key integer-family
  shapes, so the storage reader can keep raw byte ordering for this slice.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaves()`
  currently rebuilds live append-history entries into contiguous immutable
  leaf pages and publishes catalog root records.
- `packages/mylite-storage/src/storage.c::read_index_leaf_entries()`,
  `read_index_leaf_exact_entries()`, `read_index_leaf_exact_row_ids()`, and
  `find_index_leaf_exact_static_row_id()` all route through
  `read_index_leaf_run_root()`, which currently requires a
  `TABLE_INDEX_LEAF` root page and uses catalog `entry_count`.
- `packages/mylite-storage/src/storage.c::read_live_index_entries_from()` and
  the exact scan variants already provide append-tail overlays after a
  published root by scanning from the page after the published root run.

## Scope

- Publish a maintained `TABLE_INDEX_ROOT` page from the existing rebuild path
  when a rebuilt fixed-width entryset is non-empty and fits in one maintained
  root page.
- Keep existing immutable multi-page leaf runs for empty or oversized
  entrysets.
- Make full index reads, exact entry reads, exact row-id reads, and the static
  unique exact shortcut understand both root page types.
- Preserve append-tail overlay behavior by scanning index-entry pages after
  the maintained root page.

## Non-Goals

- No in-place row-DML root maintenance yet.
- No B-tree split, merge, multi-page maintained leaf, or page compaction.
- No row-state physical cleanup inside maintained root pages.
- No public `libmylite` API change.
- No MariaDB handler routing change beyond continuing to call the existing
  rebuild path.

## Design

`mylite_storage_rebuild_index_leaves()` should continue to be the publication
entry point used by the handler after supported copy-rebuild DDL. During page
preparation, each rebuilt entryset chooses one of two root shapes:

- **Maintained single-page root:** non-empty fixed-width entryset whose entry
  count fits `maintained_index_root_entry_capacity(key_size)`.
- **Immutable leaf run:** empty entryset, unsupported fixed key size, or
  entry count too large for the single root page.

The catalog root record still stores the root page pointer and the current
entry count. Reader correctness must not depend on catalog `entry_count` when
the root page is `TABLE_INDEX_ROOT`; the page's own entry count and key width
are authoritative for that root shape. This keeps current metadata inspection
stable while preparing later row-DML maintenance to update only the root page.

Readers should load the catalog root page and branch on page type:

- `TABLE_INDEX_LEAF`: keep current immutable leaf-run behavior, including
  deriving run length from catalog `entry_count`.
- `TABLE_INDEX_ROOT`: decode the maintained root, validate table and index
  ownership, expose its cell payload to existing sorted leaf-page helpers, and
  set the tail scan start to `root_page + 1`.

## Compatibility Impact

SQL-visible lookup results should not change. The maintained root uses the same
raw byte ordering already accepted for published leaf runs and falls back to
leaf runs for larger entrysets.

No compatibility matrix change is needed because this is an internal storage
layout change behind existing supported exact index behavior.

## Single-File And Lifecycle Impact

Maintained roots are written to the primary `.mylite` file by the existing
catalog publication transaction. No new persistent or transient sidecar is
introduced. Existing rollback journal coverage for append publication remains
the durability boundary for this slice.

## Test Plan

- Extend storage unit coverage so a small rebuilt exact index publishes a
  `TABLE_INDEX_ROOT` page.
- Verify full index reads and exact reads return the same row ids through the
  maintained root.
- Verify append-tail overlay after root publication still includes later
  appended rows and row-state visibility changes.
- Keep a multi-page rebuild test proving oversized entrysets still publish
  immutable `TABLE_INDEX_LEAF` runs.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

- If storage-smoke remains configured, also run:

```sh
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

## Acceptance Criteria

- Small rebuilt fixed-width indexes publish maintained root pages.
- Existing full and exact index read APIs use maintained roots without falling
  back to append-history scans.
- Append-tail overlay after maintained root publication remains correct.
- Oversized rebuilds continue to use immutable leaf runs.
- Docs and roadmap accurately describe the remaining gap: in-place row-DML root
  maintenance.

## Initial Implementation

The implementation keeps the existing storage rebuild API and handler call site
intact. Rebuild preparation now publishes a maintained single-page root for
non-empty fixed-width entrysets that fit the root-page capacity, and falls back
to immutable leaf runs for empty or oversized entrysets. Root readers load the
catalog root page, branch on `TABLE_INDEX_LEAF` versus `TABLE_INDEX_ROOT`, and
use the root page's own entry count and key width for maintained roots. The
append-tail overlay still starts at the first page after the published root.

Storage unit coverage now asserts that small rebuilds publish
`TABLE_INDEX_ROOT`, exact and full reads continue to return the expected row
ids through that root, append-tail overlays remain visible after later row
mutations, and oversized rebuilds still publish `TABLE_INDEX_LEAF` runs.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Risks

- The public root metadata API still exposes catalog entry count. This remains
  equal at publication time, but later in-place maintenance must make metadata
  reads decode maintained roots if callers require current counts.
- Reusing sorted leaf-page helper logic for a maintained root is safe only
  while the root cell layout stays identical to leaf cells.
