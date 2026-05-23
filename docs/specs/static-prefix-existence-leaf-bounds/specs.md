# Static Prefix Existence Leaf Bounds

## Goal

Use page-level prefix bounds for static durable index-prefix existence checks.
The no-tail leaf-root fast path should keep its allocation-free existence
behavior while avoiding reads of leaf pages whose last key is below the
requested serialized prefix.

## Non-Goals

- Do not change duplicate-key or prefix-index SQL semantics.
- Do not change append-tail overlay behavior.
- Do not add B-tree mutation, compaction, or a new file format.
- Do not replace byte-prefix storage comparisons with SQL collation logic.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/mylite-storage/src/storage.c:find_static_index_leaf_prefix_exists()`
  only uses the static leaf-root path when the leaf run has no append-tail
  overlay.
- `packages/mylite-storage/src/storage.c:scan_static_index_leaf_prefix_exists()`
  currently scans leaf pages from page offset `0` and stops once a page-local
  prefix comparison passes the target prefix.
- `packages/mylite-storage/src/storage.c:find_index_leaf_run_prefix_lower_page()`
  already finds the first leaf page whose last key is not below a serialized
  key prefix for prefix-entryset reads.

## Compatibility Impact

No SQL-visible behavior change. The same static leaf entries are checked with
the same `skip_row_id` semantics; only the first page read changes.

## Affected Subsystems

- Durable storage prefix-existence fast path.
- Storage tests and architecture documentation.

## Design

In `scan_static_index_leaf_prefix_exists()`:

1. ask `find_index_leaf_run_prefix_lower_page()` for the first possible
   matching page;
2. return not-found when the run cannot contain the prefix;
3. scan forward from that page using the existing page-local prefix lower bound;
4. treat a post-lower-bound key below the prefix as corruption, matching the
   stricter prefix-entryset reader invariant.

This keeps the static no-tail path allocation-free.

## File Lifecycle

No file-format or companion-file change.

## DDL Metadata Routing Impact

No metadata change. Existing index-root catalog records are reused.

## Embedded Lifecycle And API

No public API change.

## Storage-Engine Routing

MyLite-routed durable tables benefit through existing prefix-existence callers.
Volatile MEMORY/HEAP behavior is unchanged.

## Build, Size, And Dependencies

No dependency or intended size-profile change.

## Test Plan

- Keep existing static, append-tail, update, delete, skip-row, and
  missing-prefix storage coverage passing.
- Run storage unit tests, storage-smoke, clang-format diff, and `git diff
  --check`.

## Acceptance Criteria

- Static no-tail prefix-existence scans start at the first possible matching
  leaf page.
- Prefix-existence behavior and `skip_row_id` semantics remain unchanged.
- Existing storage and embedded smoke tests pass.

## Verification Results

2026-05-23, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

All passed.

## Risks And Unresolved Questions

- This is still a leaf-run page-range optimization, not a general B-tree
  navigation implementation.
