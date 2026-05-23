# Raw Prefix Index Entryset Cursors

## Problem

MyLite's handler cursor builder can already ask durable and volatile storage
for exact integer key entrysets, and storage now exposes byte-prefix entryset
readers. Composite integer-prefix index reads still used the full live index
entryset and filtered it in the handler, materializing unrelated entries before
the normal MariaDB key-tuple comparison.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  handles positive exact and prefix filters by building a filtered cursor.
- Unique full-key integer reads keep the allocation-free direct row lookup, and
  non-unique full-key integer reads use exact entryset helpers.
- `mylite_storage_read_index_prefix_entries()` and
  `mylite_volatile_read_index_prefix_entries()` return only live entries whose
  serialized key image starts with the supplied byte prefix.
- Byte-prefix filtering is safe only for key prefixes whose serialized bytes
  preserve MariaDB equality for the requested key parts. Collation-sensitive,
  nullable, and partial-key-part prefixes must stay on handler filtering with
  MariaDB key-tuple comparison.

## Design

Use the storage prefix-entryset helpers from `build_index_cursor()` only when
the supplied filter ends exactly on a complete key-part boundary and every
covered key part is a non-null integer-family field. Keep the existing exact
entryset path for full-key integer filters and keep the full-entryset fallback
for strings, nullable keys, partial key-part prefixes, and general range
neighbor reads.

The handler still validates returned entries before adding them to the cursor,
sorts the matching entries with the existing comparator, and leaves
`index_next_same()` responsible for duplicate iteration boundaries.

## Compatibility Impact

No SQL-visible behavior change is intended. The narrowed storage read is
guarded to byte-safe integer key prefixes; broader SQL collation and nullable
key semantics continue through MariaDB key-tuple comparison.

## Single-File And Lifecycle Impact

No file-format, journal, lock, catalog, sidecar, or recovery behavior changes.
The slice only changes transient handler cursor construction.

## Test Plan

- Add routed storage-engine coverage for composite integer-prefix reads through
  a forced composite secondary index, including a missing prefix.
- Run the storage-smoke embedded storage-engine test.
- Run the storage-smoke preset if focused checks pass.
- Run changed-file formatting checks and `git diff --check`.

## Acceptance Criteria

- Composite non-null integer prefix reads can use storage prefix-entryset
  helpers instead of materializing the whole index entryset.
- Full-key integer reads keep their existing exact-entryset or direct-row path.
- Collation-sensitive, nullable, partial-key-part, and range-neighbor reads keep
  the existing handler-filtered path.
- Existing routed storage tests pass.

## Verification Results

2026-05-23, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc packages/libmylite/tests/embedded_storage_engine_test.c
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

All passed.
