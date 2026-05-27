# Branch Root Index Rebuild Entrysets

## Goal

Preserve all live entries when `mylite_storage_rebuild_index_leaves()` rebuilds
an index whose current catalog root is already a maintained, leaf, or branch
root. This fixes `CREATE INDEX ... ALGORITHM=COPY` paths that rebuild the
published root after MariaDB copies rows into a replacement table.

## Non-Goals

- No new index format.
- No new public API.
- No optimizer or handler cursor change.
- No change to unsupported index types.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::create()` initializes empty
  index roots for supported keys during `CREATE TABLE`, `ALTER TABLE`,
  `CREATE INDEX`, and `DROP INDEX` rebuild-table flows.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::rename_table()` calls
  `mylite_rebuild_index_leaf_roots()` when the current SQL command rebuilt
  index roots.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_rebuild_index_leaf_roots()`
  resolves supported raw exact-filter keys from the rebuilt table share and
  delegates to `mylite_storage_rebuild_index_leaves()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaves()`
  currently builds rebuild entrysets with `read_live_index_entrysets()`.
- `packages/mylite-storage/src/storage.c::read_live_index_entrysets()` scans
  append-only index-entry pages and maintained root pages, but it ignores the
  catalog root as an authoritative source. A branch-root-backed index with
  1000 live entries can therefore rebuild from only a few stale append-entry
  pages.
- Local repro:
  `tools/mylite_perf_baseline --phase=prepared-leaf-secondary-selects 1000 10000`
  fails after `CREATE INDEX value_leaf_key ... ALGORITHM=COPY` because
  `mylite_storage_read_index_root()` reports 3 entries for the secondary root
  while the table has 1000 rows.

## Compatibility Impact

SQL-visible behavior should only become more correct: rebuilt secondary and
primary index roots retain the live entries MariaDB copied into the replacement
table. `docs/COMPATIBILITY.md` does not need a new compatibility claim because
indexed reads after copy `ALTER` / `CREATE INDEX` are already covered as
supported routed DDL/DML behavior.

## Design

Teach the rebuild-entryset reader to prefer the current catalog index root for
each requested index when one exists. The reader should:

- resolve the catalog root record for each rebuild target;
- read entries from maintained, leaf-run, or branch roots through the existing
  root readers, including any append-tail overlay they already merge;
- mark that rebuild target as sourced from a catalog root; and
- keep the old append-entry scan only for rebuild targets without a current
  root record.

The old scan path remains necessary for legacy and storage-level calls that ask
for a leaf rebuild before a root has ever been published.

## File Lifecycle

The slice only changes how existing pages are read before publishing a new root.
It keeps durable data in the primary `.mylite` file and uses the existing
journaled publish path. No new companion files are introduced.

## Embedded Lifecycle And API

No public embedded lifecycle or C API behavior changes.

## Build, Size, And Dependencies

No new dependency or build-profile change. The code change is first-party
storage logic only.

## Test Plan

- Add a storage regression that inserts enough rows in one transaction to
  publish branch-backed maintained roots, verifies the full secondary entry
  count, rebuilds both roots, and verifies the rebuilt roots still expose that
  full count.
- Extend the active packed branch-root insert test to assert root metadata
  before and after rebuild.
- Re-run the failing `prepared-leaf-secondary-selects` performance phase.
- Run focused storage tests, format checks, and diff checks.

## Acceptance Criteria

- Branch-root-backed rebuilds preserve the full root entry count.
- Rebuilds without an existing root still work through the append-entry scan.
- The published secondary benchmark phase no longer fails its root-entry
  verification.

## Initial Implementation

- `mylite_storage_rebuild_index_leaves()` now passes the current filename,
  catalog, schema, and table identity into the rebuild-entryset reader.
- Rebuild targets with a current catalog root load entries through
  `read_index_leaf_entries()`, so maintained roots, leaf runs, branch roots,
  and their existing append-tail overlays use the same authoritative root
  reader as normal index reads.
- Rebuild targets without a catalog root keep the previous append-entry scan.
- The active branch-root packed insert regression now rebuilds both roots and
  verifies that primary and secondary root metadata still report the full row
  count.
- Benchmark labels now say "published-root" for user-facing rows because the
  validated root may be a maintained, leaf, or branch root.

## Verification Results

Passed:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-leaf-secondary-selects 1000 10000
```

## Risks And Open Questions

- Leaf-run and branch-root readers already merge tail overlays. If a future
  root type needs different overlay semantics, this reader must use the new
  root-specific merge path instead of scanning append pages blindly.
