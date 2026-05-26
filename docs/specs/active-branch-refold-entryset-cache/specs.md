# Active Branch Refold Entryset Cache

## Problem

Prepared insert profiling after prevalidated branch snapshot leaf writes shows
that live-overlay branch refold planning still rebuilds the same full branch
entryset across repeated active inserts. The hot path is
`plan_maintained_index_root_inserts()` ->
`try_plan_branch_index_root_refold_insert()` ->
`build_branch_index_refold_insert_entryset_if_fit()`, which calls
`read_index_leaf_entries()` and then rereads branch leaves, hashes live row ids,
and appends the candidate row before deciding whether the refold fits.

The previous slice carries the planning-built entryset into execution for a
single row insert. This slice makes the next planning pass reuse the final
post-refold entryset when the same active statement inserts again into the same
branch root.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code and does not change MariaDB-derived SQL, handler, or
  parser code.
- `packages/mylite-storage/src/storage.c::plan_branch_index_root_insert()`
  reaches the refold planner only for level-`1` branch roots when local insert,
  split, and redistribution paths cannot cheaply maintain the child leaf.
- `packages/mylite-storage/src/storage.c::build_branch_index_refold_insert_entryset_if_fit()`
  currently rebuilds the whole live entryset with `read_index_leaf_entries()`
  for each planned refold.
- `packages/mylite-storage/src/storage.c::refold_branch_index_root_insert()`
  already accepts the planning-built `refold_entryset`, so execution avoids a
  second root entryset read when planning supplied one.
- Existing active statement caches for branch-tail overlays, branch pages, and
  leaf pages are cleared on rollback, catalog invalidation, and reusable
  statement reset. The new cache should follow the same lifecycle.

## Design

Add a bounded active statement cache keyed by branch root page, table id, index
number, key size, and branch entry count. A cache entry owns a deep copy of the
final post-refold `mylite_storage_index_entryset`.

Planning will:

1. Check the active cache before `read_index_leaf_entries()`.
2. Accept a hit only when the active branch page still has matching table,
   index, key size, level, and entry count.
3. Copy the cached entryset, append the proposed row/key to the copy, and run
   the existing fit calculation unchanged.
4. Fall back to the current full read path on a miss.

Mutation maintenance will:

1. Clear branch-refold entryset caches on generic table mutation, catalog
   invalidation, rollback, statement free, and reusable statement reset.
2. After a successful insert, repopulate matching cache entries only for
   branch-refold plan entries that carried a planning-built entryset.
3. Remove non-refold branch insert entries from the cache so direct branch
   rewrites, splits, and redistributions cannot leave stale cached contents.

The cache is transient process-local state. It changes no file format and is
not shared across connections or processes.

## Compatibility Impact

No SQL-visible or public API behavior changes. `ENGINE=InnoDB`, explicit
MyLite, and other routed-engine paths continue to use the same MyLite storage
maintenance logic; this only avoids repeated in-process entryset rebuilds when
the existing branch-refold path is selected.

## Single-File And Lifecycle Impact

The cache owns heap memory attached to active statements. It introduces no
persistent sidecar and no durable file-format state. Rollback and catalog
invalidation clear parent-chain cache entries so nested statement rollback
cannot leave stale parent state.

## Binary-Size And Dependency Impact

No dependency changes. The code adds small cache-management helpers and test
hooks in first-party storage code only.

## Test And Verification Plan

- Add a storage test hook that counts branch refold entryset read-path rebuilds.
- Extend maintained branch-root storage coverage so live-overlay refold
  planning still builds the first entryset from storage exactly once.
- Add a direct cache roundtrip hook that proves root-owner lookup, matching
  branch snapshot cache hits, entry-count mismatch misses, and table
  invalidation.
- Run `mylite_storage_test`, `ctest -R mylite-storage`, storage-smoke embedded
  tests, formatting checks, and the prepared insert component baseline.

## Acceptance Criteria

- Cache hits produce the same branch root, leaf pages, lookup results, rollback
  behavior, and file size checks as the uncached path.
- Any non-refold mutation clears or removes affected cache entries.
- Test hooks prove matching active cache hits avoid the full refold entryset
  read helper, while the maintained branch-root regression keeps the live
  refold path covered.
- Relevant builds, tests, and formatting checks pass.

## Risks

- A stale cache would reintroduce deleted or updated index entries. The first
  implementation intentionally clears on generic table mutation and stores only
  after successful refold inserts to keep the cache conservative.
- The cache only helps repeated refolds on active statements. Cold reads and
  unrelated branch maintenance still need the broader pager/WAL and navigable
  B-tree work already tracked on the roadmap.
