# Volatile Index Prefix Entryset Read

## Goal

Add a volatile row-store helper that returns live index entries matching a
serialized key prefix, and use it for grouped later-in-key `AUTO_INCREMENT` on
runtime-volatile `MEMORY` / `HEAP` tables.

## Non-Goals

- Do not change durable storage behavior.
- Do not change volatile row snapshot, transaction, or savepoint semantics.
- Do not add sorted volatile indexes; this remains a filtered scan over the
  current process-local row vectors.
- Do not change SQL-visible grouped autoincrement behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc:mylite_read_grouped_auto_increment()`
  now asks durable storage for a prefix entryset but still asks volatile
  storage for a full index entryset.
- `mariadb/storage/mylite/mylite_volatile_rows.cc:mylite_volatile_read_index_entries()`
  returns all live entries for one volatile table/index.
- `mariadb/storage/mylite/mylite_volatile_rows.cc:mylite_volatile_read_exact_index_entries()`
  already narrows volatile entrysets by exact serialized key bytes.
- Runtime-volatile rows are process-local and already filtered for `deleted`
  row state in the volatile row vector.

## Compatibility Impact

SQL-visible behavior should not change. The handler still compares full
MariaDB key tuples for matching prefix entries before selecting the grouped
autoincrement maximum.

## Affected Subsystems

- Volatile row store index-entry API.
- Grouped later-in-key autoincrement allocation for `MEMORY` / `HEAP`.
- Embedded storage-engine smoke coverage.

## Design

Add `mylite_volatile_read_index_prefix_entries()` beside the full and exact
volatile entryset readers.

The helper will:

1. validate table, prefix, and output arguments;
2. initialize the growable `mylite_storage_index_entryset`;
3. scan live non-deleted volatile rows under the existing volatile mutex;
4. count and copy only entries whose index number matches and whose serialized
   key starts with the requested prefix;
5. return full key images so the handler can keep using MariaDB key comparison
   for maximum selection.

Then route the volatile branch of `mylite_read_grouped_auto_increment()` through
the new helper.

## File Lifecycle

No durable file-format or companion-file change. Volatile rows remain
process-local and disappear on embedded runtime shutdown.

## DDL Metadata Routing Impact

No metadata change. Existing `MEMORY` / `HEAP` requested-engine metadata remains
durable while rows stay volatile.

## Embedded Lifecycle And API

No public `libmylite` API change. The helper is internal to the MyLite handler
and volatile row store.

## Storage-Engine Routing

Runtime-volatile `MEMORY` / `HEAP` grouped autoincrement uses the narrowed
entryset helper. Durable MyLite-routed engines keep the durable prefix-entryset
path.

## Build, Size, And Dependencies

No dependency or intended size-profile change.

## Test Plan

- Add embedded storage-engine coverage for grouped later-in-key autoincrement
  on a `MEMORY` table, including per-prefix allocation and live-prefix behavior
  after explicit high rows and deletes.
- Run storage-smoke, clang-format diff, and `git diff --check`.

## Acceptance Criteria

- Volatile grouped autoincrement no longer asks for a full volatile index
  entryset.
- The new volatile helper returns only live entries with the requested
  serialized key prefix while preserving full key images.
- Embedded storage-engine tests pass.

## Verification Results

2026-05-23, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- mariadb/storage/mylite/mylite_volatile_rows.h mariadb/storage/mylite/mylite_volatile_rows.cc mariadb/storage/mylite/ha_mylite.cc packages/libmylite/tests/embedded_storage_engine_test.c
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

All passed.

## Risks And Unresolved Questions

- The helper still scans volatile row vectors. A sorted volatile index remains
  future work if runtime-volatile tables need stronger performance.
