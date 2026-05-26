# Branch Refold Cache Simple Insert Maintenance

## Problem

Fresh prepared-insert profiling shows `read_index_leaf_entries()` under branch
refold planning as a remaining hotspot. The branch refold entryset cache avoids
that read after a refold, but ordinary inserts into an existing branch leaf
currently remove the cache even though the branch root shape is unchanged.

That means later refold planning can reread and decode the whole branch leaf
set even when MyLite already had the previous full entryset in memory and the
successful insert only appended one new logical entry.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `update_branch_refold_entryset_caches_after_branch_insert()` removes cached
  branch refold entrysets for every branch insert, then stores a new cache only
  for `refold_branch` plans with a prepared `refold_entryset`.
- A non-splitting, non-redistributing, non-refolding level-one branch insert
  rewrites one child leaf plus its root fence and increments the branch entry
  count; child page ids and branch shape are unchanged.
- The cache key validates level, key width, root id, table id, index number,
  and entry count against the next decoded branch page before reuse.

## Design

Maintain an existing branch refold entryset cache after simple branch inserts:

- Add the inserted `(key, row_id)` to the cached entryset when the branch insert
  does not split, redistribute, or refold the branch.
- Increment the cached branch entry count so the next lookup can match the
  updated branch page.
- Keep structural branch operations on the existing remove/store path.
- Treat cache maintenance as opportunistic. If the cached entry looks
  inconsistent or allocation fails, remove the cache instead of failing the row
  insert.

## Compatibility Impact

No SQL-visible, API-visible, storage-routing, or durable file-format behavior
changes. The cache only affects transient planning work before writing the same
leaf and branch pages.

## Single-File And Lifecycle Impact

No checkpoint, transaction, journal, lock, recovery, or companion-file changes.
Rollback and cache invalidation remain governed by the active statement cache
lifecycle.

## Binary-Size And Dependency Impact

No dependency changes. The slice adds a small first-party cache maintenance
helper.

## Test And Verification Plan

- Extend branch refold cache hook coverage so a simple branch insert updates an
  existing cache and the next lookup uses the incremented entry count.
- Keep the stale entry-count miss and table-level invalidation coverage.
- Run storage unit tests, storage-smoke embedded tests, formatting checks, and
  a prepared-insert component benchmark.

## Acceptance Criteria

- Simple branch inserts preserve and append existing branch refold entryset
  caches.
- Structural branch insert paths still remove or replace caches.
- Cache reuse still requires matching branch metadata and entry count.
- Relevant tests and formatting checks pass.

## Risks

The maintained cache entry may be unsorted after appending a non-tail key. That
is acceptable because refold snapshot preparation still sorts out-of-order
entrysets before encoding durable leaf pages.
