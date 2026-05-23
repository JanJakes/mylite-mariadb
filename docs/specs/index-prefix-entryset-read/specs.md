# Index Prefix Entryset Read

## Goal

Add a durable storage helper that materializes live index entries matching one
serialized key prefix, and use it for grouped later-in-key `AUTO_INCREMENT`
allocation. This keeps grouped allocation on the live-prefix model while
avoiding handler-side filtering of the whole index entryset when a narrower
storage read is enough.

## Non-Goals

- Do not add B-tree splits, maintained multi-page mutation, or a new file
  format.
- Do not change volatile MEMORY/HEAP entryset reads.
- Do not change first-key table-local autoincrement behavior.
- Do not add collation-aware storage comparisons; the helper operates on
  MariaDB-built serialized key bytes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc:mylite_read_grouped_auto_increment()`
  serializes the non-autoincrement key prefix, reads the full index entryset,
  filters that entryset in the handler, then chooses the maximum matching row.
- `packages/mylite-storage/src/storage.c:mylite_storage_read_index_entries()`
  can already use catalog-backed leaf roots as a base snapshot and overlay
  append-tail row-state/index-entry pages.
- `packages/mylite-storage/src/storage.c:read_index_leaf_exact_entries()`
  narrows exact-key leaf reads before applying append-tail overlay.
- `packages/mylite-storage/src/storage.c:mylite_storage_index_prefix_exists_for_index()`
  already proves that serialized key prefixes can be answered from static leaf
  roots and fall back to entryset-based scanning when a tail overlay is needed.
- `packages/mylite-storage/src/storage.c:find_index_leaf_page_prefix_lower_bound()`
  provides a page-local lower bound for shorter-than-full-key prefixes.

## Compatibility Impact

SQL-visible behavior should not change. The grouped autoincrement handler still
selects the maximum matching live key tuple through MariaDB key comparison and
fetches that row before returning the next generated value.

## Affected Subsystems

- First-party durable storage index read API.
- Durable leaf-root and append-tail overlay readers.
- MyLite handler grouped `AUTO_INCREMENT` allocation.
- Storage unit tests and embedded storage-engine smoke coverage.

## Design

Add `mylite_storage_read_index_prefix_entries()` with schema, table, index
number, serialized key prefix, and output entryset arguments.

The helper will:

1. validate arguments and initialize `out_entries`;
2. open the current durable file through the same scoped read path as full and
   exact entryset reads;
3. resolve the table and index-root metadata using existing active statement
   caches;
4. when a published leaf root exists, use a page-level prefix lower bound,
   append only matching leaf entries using page-local prefix lower-bound search,
   and then scan the append tail for matching live entries;
5. when no root exists, scan the live append history but append only entries
   whose key starts with the requested prefix;
6. preserve the existing row-state replacement/delete overlay semantics.

The returned entryset keeps complete key images, not truncated prefixes, so the
handler can continue to compare full key tuples and choose the maximum matching
row exactly as before.

## File Lifecycle

No file-format or companion-file change. The helper reads existing row-state,
index-entry, maintained-root, and leaf-run pages from the primary `.mylite`
file.

## DDL Metadata Routing Impact

No DDL metadata change. Existing index-root records are reused when available.

## Embedded Lifecycle And API

No public `libmylite` API change. The first-party storage API gains a durable
entryset helper used internally by the MyLite handler.

## Storage-Engine Routing

Durable MyLite-routed grouped autoincrement tables use the prefix helper.
Runtime-volatile MEMORY/HEAP tables keep `mylite_volatile_read_index_entries()`
until a matching volatile prefix helper is justified.

## Build, Size, And Dependencies

No dependency or intended size-profile change. Binary impact should be limited
to small first-party storage and handler helpers.

## Test Plan

- Add storage unit coverage for prefix entryset reads against:
  - static published leaf roots;
  - append-tail inserts after a published root;
  - changed-key updates and deletes through row-state overlay;
  - missing prefixes;
  - shorter-than-full-key prefixes.
- Keep grouped autoincrement embedded coverage passing.
- Run storage unit tests, the focused embedded storage-engine smoke, the full
  storage-smoke preset, clang-format diff, and `git diff --check`.

## Acceptance Criteria

- Durable prefix entryset reads return the same live matching rows that callers
  would get by reading the full entryset and filtering by prefix.
- Published leaf roots avoid materializing unrelated base entries and can start
  at the first leaf page whose last key is not below the requested prefix.
- Append-tail and missing-root paths keep correctness through row-state
  replacement/delete overlays.
- Grouped durable autoincrement calls the prefix entryset helper.
- Existing storage and embedded tests pass.

## Verification Results

2026-05-23, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

All passed.

## Risks And Unresolved Questions

- This is still not a navigable B-tree. Prefix reads can avoid unrelated leaf
  entries and unrelated append entries, but they still scan append-tail pages.
- The helper is byte-prefix based, which is correct for MariaDB-built key
  images but not a general SQL collation API.
