# Static Leaf Prefix Exists

## Problem

Foreign-key parent and child existence checks ask whether any row in a specific
index has a key with a given prefix, optionally excluding the row currently
being updated. The handler currently calls `mylite_storage_read_index_entries()`
and scans the full materialized entryset even when the index has a static
published leaf root that can answer the boolean from sorted leaf pages.

That keeps FK checks correct, but it adds avoidable entryset allocation and
row-id/key copying on hot DML paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_index_prefix_exists()` reads a
  full durable index entryset and scans it for FK prefix checks.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_child_foreign_key_parent_prefix_exists()`
  and `mylite_parent_foreign_key_child_prefix_exists()` call that helper for
  immediate FK row checks.
- `packages/mylite-storage/src/storage.c::read_index_leaf_run_root()` already
  validates catalog-backed leaf roots and maintained roots, computes a
  `tail_page_id`, and tells callers whether pages appended after publication
  must be overlaid.
- Static leaf roots with `tail_page_id == header.page_count` are complete live
  snapshots for the index, so a prefix-exists check can scan those sorted leaf
  pages directly without materializing an entryset.

## Scope

- Add an index-specific durable storage prefix-exists API with optional row-id
  exclusion.
- Use static published leaf roots and non-overflow maintained roots directly
  when they are complete snapshots.
- Keep append-tail roots, missing roots, and unsupported shapes on the existing
  materialized entryset fallback.
- Route durable handler FK prefix checks through the new storage API.

## Non-Goals

- No volatile MEMORY/HEAP path change.
- No B-tree split, maintained multi-page mutation, or free-space work.
- No collation-sensitive prefix comparison change. The helper compares the
  serialized key bytes already used by existing FK prefix checks.
- No change to FK action semantics or metadata publication.

## Design

Add `mylite_storage_index_prefix_exists_for_index()` with schema, table, index
number, key prefix, optional skipped row id, and boolean output.

The function opens the durable file, reads the current header and catalog, and
first asks a static leaf helper to resolve the prefix:

- if no catalog root exists, it declines the fast path;
- if the root has an append tail, it declines the fast path so row-state and
  later index entries are overlaid by the existing fallback;
- if the root is static, it scans the sorted leaf pages directly, returns true
  on the first matching non-skipped row id, and stops early once page order has
  advanced past the requested prefix.

The fallback reuses `read_index_leaf_entries()` when a leaf root exists and
`read_live_index_entries()` otherwise, then scans the materialized entryset with
the same prefix and skipped-row rules as the handler currently applies.

## Compatibility Impact

SQL-visible FK behavior should not change. The fast path applies only when the
published root is the complete live snapshot. Roots with append-tail overlays
still use the existing materialized path.

## Single-File And Lifecycle Impact

No file-format or lifecycle change. The helper reads existing `.mylite` pages
and introduces no companion files.

## Public API And File-Format Impact

The first-party storage API gains an index-specific prefix-exists helper. The
file format is unchanged.

## Storage-Routing Impact

Durable MyLite-routed FK checks can avoid full index-entry materialization for
static published roots. Volatile routed tables keep their existing in-memory
entryset path.

## Binary-Size, License, And Dependency Impact

No imported code or dependency change. Binary impact is limited to small
first-party storage scan helpers and a handler call-site simplification.

## Test Plan

- Extend storage unit coverage over a multi-page published leaf root:
  - prefix found on the first, later, and last pages;
  - missing prefix;
  - skipped row id hides a single matching row.
- Cover single-page maintained roots across insert, update, delete, and skipped
  row id checks.
- Verify append-tail roots still preserve prefix visibility through the
  fallback path.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc
```

## Acceptance Criteria

- Static published leaf roots can answer index-specific prefix-exists checks
  without materializing a full entryset.
- Append-tail roots and missing roots keep the existing correctness path.
- Durable FK prefix checks call the storage helper rather than scanning a
  handler-owned full entryset.

## Verification Results

2026-05-21, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

All passed. The storage-smoke MariaDB archive remained
`build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`,
`size_bytes=21101384`, `size_mib=20.12`, `members=481`.

During storage-smoke verification, maintained-root FK paths exposed an existing
statement recovery-journal limitation: autoincrement metadata could open the
journal before row append needed to protect a maintained index root. That was
fixed separately by allowing statement recovery journals to be rewritten for
late dirty pages.

## Risks And Open Questions

- This is a read-path optimization, not a replacement for maintained B-tree
  navigation.
- The fast path intentionally does not cover append-tail roots because deletes,
  replacements, and changed-key entries after root publication can alter the
  answer.
