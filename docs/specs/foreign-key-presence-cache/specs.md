# Foreign Key Presence Cache

## Problem

The routed storage benchmark updates a table with no foreign keys, but the
handler still asks storage to scan child and parent foreign-key metadata for
each row write, update, delete, and `referenced_by_foreign_key()` probe.
Sampling after the durable row and leaf-page read caches showed repeated
catalog reads and checksums in those no-op metadata checks during update-heavy
paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::write_row()` calls
  `mylite_check_child_foreign_keys()` before appending durable rows.
- `ha_mylite::update_row()` calls child FK checks, same-row parent update
  actions, parent actions, and parent checks before updating durable rows.
- `ha_mylite::delete_row()` calls parent actions and parent checks before
  deleting durable rows.
- `ha_mylite::referenced_by_foreign_key()` calls
  `mylite_storage_list_parent_foreign_keys()` to answer MariaDB metadata
  queries.
- `packages/mylite-storage/src/storage.c::mylite_storage_list_foreign_keys()`
  and `mylite_storage_list_parent_foreign_keys()` open the primary file and
  read the header and catalog root even when no matching FK metadata exists.

## Design

- Add handler-instance child and parent FK-presence caches.
- Populate the child cache with a single storage FK-list presence probe for the
  opened table. Populate the parent cache with a single parent-FK presence
  probe for the opened table.
- Reuse the existing presence callback's short-circuit behavior, accepting the
  storage-layer `ERROR` result only when the callback has positively found FK
  metadata.
- Skip expensive child or parent FK row-check/action paths only when the
  corresponding cache has proven there is no matching FK metadata.
- Treat storage errors conservatively: row-DML paths return the mapped handler
  error, while `referenced_by_foreign_key()` keeps MariaDB's existing safe
  behavior by reporting the table as referenced on probe failure.
- Clear the cache on open and close. Successful local table-DDL mutations bump
  a process-wide FK metadata epoch so already-open handlers refresh their
  presence caches lazily before later DML or metadata probes.
  Existing MariaDB metadata locking remains the authority for cross-process DDL
  visibility.

## Compatibility Impact

SQL-visible FK behavior is unchanged. Tables with child or parent FK metadata
continue down the existing enforcement and action paths. The optimization only
removes repeated storage catalog scans after a handler has proven that the
opened table has no relevant FK metadata.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The cache is process memory
attached to a handler instance and is cleared across handler lifecycle changes.

## Test And Verification Plan

- Existing storage-engine FK tests cover child checks, parent checks, metadata
  hooks, `referenced_by_foreign_key()` behavior, and FK actions.
- Rebuild the storage-smoke embedded target.
- Run the storage-engine compatibility harness.
- Run the local performance baseline to measure update-path impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Non-FK routed tables avoid repeated child and parent FK catalog-listing work
  after the first handler presence probe.
- Existing FK enforcement and metadata tests continue to pass.
- Update-heavy benchmark timings improve materially without changing row or FK
  behavior.

## Risks

- The cache intentionally relies on MariaDB metadata-lock lifetimes plus a
  process-local FK metadata epoch for DDL visibility. A future shared-table or
  storage-level metadata cache may be a better fit once MyLite has broader SQL
  lock integration.
