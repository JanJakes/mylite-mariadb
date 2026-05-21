# Maintained Index Root Pages

## Goal

Introduce the page-format boundary for mutable maintained indexes without
republishing catalog metadata for every row mutation. Existing published leaf
runs store the total entry count in the catalog index-root record, which is
fine for immutable rebuild snapshots but wrong for maintained pages: an insert
or delete would have to update the catalog record every time.

The next maintained-index foundation should add a root page whose page body
owns mutable index state such as entry count, fixed key width, and the current
single-page leaf payload. The catalog keeps only a stable root-page pointer for
the index.

## Non-Goals

- No full B-tree split, merge, or multi-level navigation.
- No broad collation-sensitive or nullable-key ordering.
- No production row-DML index maintenance in this slice unless the root page
  and journaling contract are already in place and tests prove the first
  insert-only path.
- No transaction-journal redesign.
- No WAL, lock-manager, MVCC, or cross-process write-concurrency claim.
- No public `libmylite` API change.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_uses_raw_exact_filter()`
  limits storage-level byte-exact lookup to guarded full-key integer-family
  shapes. General MariaDB key ordering remains handler-owned.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaf()`
  publishes immutable sorted leaf runs and stores `root_page` plus
  `entry_count` in catalog index-root metadata.
- `packages/mylite-storage/src/storage.c::read_index_leaf_run_root()` derives
  run length and per-page expected entry counts from catalog `entry_count`.
  That makes current leaf runs unsuitable for in-place mutation without a
  catalog update.
- `packages/mylite-storage/src/storage.c::begin_write_journal_for_statement_pages()`
  can now create an active recovery journal from a bounded preplanned dirty
  page set before a row mutation appends new pages.
- `packages/mylite-storage/src/storage.c::pager_write_page()` now captures
  active in-memory dirty-page preimages and rejects unprotected dirty writes
  once the recovery journal is immutable.

## Compatibility Impact

No SQL-visible behavior should change at the page-format foundation stage.
When production maintenance is added, it must preserve the same exact-key
lookup results, duplicate-key semantics, row-state visibility, and handler
guards as the existing append-only index-entry plus leaf-run overlay path.

`docs/COMPATIBILITY.md` should stay unchanged until a user-visible SQL path
uses maintained root pages.

## Design

Add a new maintained index root page under the existing MyLite index page
family. The root page is distinct from immutable leaf-run pages:

- same index magic and format checksum family;
- page type `TABLE_INDEX_ROOT`;
- root page id;
- table id;
- MariaDB key number;
- fixed raw key size;
- mutable entry count;
- used bytes;
- flags for supported root shape;
- sorted `(row id, key bytes)` cells for the initial single-page root shape.

The first root shape is deliberately a single page:

- supports only fixed-width raw byte keys that already pass existing exact
  storage lookup guards;
- stores entries sorted by raw key bytes and row id;
- can be rewritten in place when the new entry fits;
- falls back to append-only index-entry pages when full or unsupported; and
- does not change catalog metadata after the root pointer is first published.

Current immutable leaf runs remain valid and keep their catalog-owned
`entry_count` semantics. Readers must distinguish the page type:

- `TABLE_INDEX_LEAF`: use existing immutable leaf-run metadata rules;
- `TABLE_INDEX_ROOT`: use the root page's own entry count and key width.

Production row-DML maintenance should only target a maintained root when:

- the catalog root points at a valid maintained root page;
- the root has no unsupported flags;
- the key size matches the incoming raw key image;
- the root has room for the inserted cell;
- the active statement can pre-register the root page in the recovery journal;
  and
- the mutation can avoid appending a duplicate index-entry page for the same
  maintained key image.

Update and delete support can build on row-state visibility first: a maintained
root may continue to contain older row ids while appended row-state pages hide
them. Physical removal, compaction, and page splits belong to later slices.

## File Lifecycle

Maintained root pages live in the primary `.mylite` file. Dirty root rewrites
are protected by the existing transient recovery journal and active in-memory
dirty-page undo lists. No persistent sidecar files are introduced.

## Public API And File-Format Impact

No public `libmylite` API change. The primary file gains a new internal page
type while keeping the early-development storage format version unchanged.
Catalog index-root records keep the stable root pointer; the `entry_count`
field remains authoritative only for immutable leaf-run roots.

## Storage-Engine Routing Impact

No routing change. Maintained roots apply only after a durable MyLite-routed
table and supported key shape already exist.

Runtime-volatile `MEMORY` / `HEAP` tables keep process-local rows and should
not publish maintained durable root pages.

## Build, Size, And Dependencies

No new dependency and no MariaDB-derived source change. Binary-size impact
should be limited to root-page encode/decode helpers, lookup branching, and
focused tests.

## Test Plan

- Add storage unit tests for encoding and decoding a maintained root page with
  zero, one, and multiple fixed-width entries.
- Verify invalid page ids, table ids, key sizes, used bytes, entry ordering,
  and checksums are rejected.
- Verify existing immutable leaf-run tests keep using the old page type and
  catalog `entry_count` semantics.
- If production insert maintenance is included, add statement rollback and
  crash-recovery tests that pre-register the root page, append a row, rewrite
  the root, and prove no duplicate append-tail index entry appears.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c
```

- If storage-smoke remains configured, also run:

```sh
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

## Acceptance Criteria

- Maintained root page bytes have a documented, typed, checksummed format.
- Decode validates ownership, key width, used bytes, sorted cell order, and
  row-id addressability.
- Existing immutable leaf-run behavior remains unchanged.
- The next production insert-maintenance slice can depend on root-page-owned
  mutable entry counts rather than catalog publication per row.

## Initial Implementation

The first implementation adds the maintained root page constants and private
encode/decode helpers. The page stores a single-page root shape with
root-owned entry count, fixed key width, flags, used bytes, and sorted
`(row id, key bytes)` cells. Decode validates checksum, root ownership, flags,
used bytes, row-id addressability, and strict key/row-id ordering.

Storage unit coverage uses build-testing-only hooks to encode and decode
valid one-entry, multi-entry, and empty roots, then rejects checksum
corruption, ownership mismatches, invalid row ids, invalid key-size and
used-bytes fields, unsupported flags, and unsorted cell order. Existing
immutable leaf-run readers are unchanged, and other decoders now treat the
new root page type as a known non-target page.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Risks And Open Questions

- A single-page root is only a stepping stone. Larger indexes still need page
  splits or a multi-page root/leaf structure.
- Production readers now distinguish immutable leaf-run roots from maintained
  root pages by reading the page type; broader B-tree shapes still need the
  same explicit dispatch.
- Omitting append-only index-entry pages for maintained inserts requires careful
  fallback rules so corruption or unsupported shapes do not lose index
  visibility.
- Transaction-aware dirty-page protection remains planned for active durable
  transactions.
