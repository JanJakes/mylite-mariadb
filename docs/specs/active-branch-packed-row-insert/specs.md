# Active Branch Packed Row Insert

## Problem

Prepared indexed inserts now pack fixed-size rows while maintained roots remain
single-page roots, but the writer falls back to legacy one-row pages when a
maintained index root is full, has an overflow tail, or has promoted to a
branch root. The local prepared insert component benchmark shows that fallback
dominates file growth: in a 10,000 prepared-insert sample, the final file had
10,332 legacy row pages and only 46 packed row pages.

The branch and overflow index writers already treat row ids as opaque
references. The row writer should keep using marked packed row references when
the existing maintained-index planner can either mutate the branch/root pages
or deliberately leave an append-tail overlay.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:3929-4086` prepares index entries before
  calling `mylite_storage_append_row_with_index_entries()` and stores the
  returned 64-bit row id as the handler row reference.
- `mariadb/storage/mylite/ha_mylite.cc:3628-3699` materializes rows from that
  opaque row reference in `rnd_pos()`.
- `mariadb/sql/handler.cc:7547` and `mariadb/sql/handler.cc:8172` use handler
  `position()` row references as opaque bytes for later row lookup and
  comparison, so MyLite can keep using marked packed references internally.
- `packages/mylite-storage/src/storage.c::maintained_index_roots_allow_packed_insert()`
  currently disables packed inserts when a maintained root is full or has
  become a branch root.
- `packages/mylite-storage/src/storage.c::plan_branch_index_root_insert()`,
  `write_branch_index_root_inserts()`, and the maintained overflow-tail path
  pass the inserted row id through without assuming it is an unmarked physical
  row page id.
- `packages/mylite-storage/src/storage.c::read_index_branch_entries()` falls
  back to the live append-tail scan when durable pages exist after a branch
  root's covered page range, preserving visibility for planned append-only
  fallback entries.
- Append-only index-entry, maintained-root, branch, and leaf readers already
  validate row ids through the packed-reference-aware row-reference helpers.

## Design

- Allow active packed-row insertion for maintained roots that are full,
  overflow-marked, cached as active branch pages, or durable branch pages.
- Keep the row writer guarded by the existing packed preconditions:
  - active append-buffer statement;
  - fixed-size inline row payload;
  - append-buffer reservation without flushing;
  - valid packed page/slot reference.
- Do not force branch insertion. The existing maintained-index planner remains
  authoritative:
  - if branch/root planning succeeds, it clears the matching
    `index_entry_changed` bit and the packed row id is written into the
    maintained page;
  - if the planner intentionally leaves the bit set, the packed writer emits
    append-only index-entry pages with the same marked row id, and existing
    tail-overlay reads preserve visibility.
- Keep maintained-index planning and row writing on the same row reference:
  start with the conservative append-page prediction, plan the maintained
  branch/root writes, recompute the append shape from the resulting fallback
  index-entry count, and replan only when that changes the predicted row id.
  This is required for duplicate-heavy secondary indexes because branch leaf
  selection is ordered by `(key, row_id)`.
- Keep non-active direct inserts, oversized rows, and append-buffer flush
  fallbacks on the legacy writer.

## Affected Subsystems

- Active fixed-size insert row-id prediction.
- Maintained-root overflow and branch-root insert routing.
- Packed row page append cache.
- Exact indexed lookup and full index entry reads for branch roots with packed
  row ids.

## Compatibility Impact

No SQL-visible behavior change is intended. `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, and default routed durable tables still resolve to MyLite
storage under the embedded profile. The change only affects MyLite's internal
row-reference shape for eligible fixed-size active inserts.

## Single-File And Lifecycle Impact

No new sidecars or lifecycle states. Packed row pages, branch/leaf/root
rewrites, and append-only fallback entries remain ordinary pages inside the
primary `.mylite` file, protected by the existing statement or transaction
journal where dirty existing pages are touched.

## Public API And File-Format Impact

No public API signature change. The existing row-page version `2` and marked
packed row-reference encoding are used in more active insert cases.

## Storage-Engine Routing Impact

No routing policy change. The slice improves routed durable storage behavior
after maintained indexes promote out of single-page roots.

## Binary-Size Impact

Small first-party storage changes only. No dependency change.

## Tests And Verification Plan

- Extend packed maintained-root storage coverage so rows inserted after the
  root reaches capacity still return marked packed references.
- Add/extend branch-root storage coverage so active branch-root inserts:
  - return marked packed row references;
  - reuse the same packed physical row page across a transaction and nested
    savepoint;
  - remain visible through exact indexed lookup before and after commit;
  - roll back packed slots and branch visibility on statement, savepoint, and
    transaction rollback.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

## Acceptance Criteria

- Active fixed-size maintained-root overflow inserts can keep using marked
  packed row ids.
- Active fixed-size branch-root inserts can keep using marked packed row ids.
- Exact indexed lookup and full index reads materialize packed rows through
  branch and overflow paths.
- Existing storage and routed storage-engine tests pass.
- Prepared insert benchmark file growth drops materially for the focused sample
  without regressing correctness.

## Risks And Unresolved Questions

- This does not implement arbitrary B-tree balancing or free-space reuse.
- Branch planners that still choose append-tail fallback keep the index visible
  but may retain slower read paths until broader maintained B-tree maintenance
  lands.
- BLOB/TEXT and variable-size packed rows remain out of scope.

## Verification Results

- Added storage coverage for duplicate-heavy maintained secondary indexes where
  active branch-root planning must use the final packed row reference.
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  completed successfully with 893 final pages and 3,657,728 final bytes for the
  focused local sample.
