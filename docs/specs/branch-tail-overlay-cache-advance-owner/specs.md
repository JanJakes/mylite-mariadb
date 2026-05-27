# Branch Tail Overlay Cache Advance Owner

## Problem

The prepared-insert benchmark now reports branch-maintenance counters. A
10,000-row local run showed zero branch refold entryset reads but still reported
branch-tail overlay scans and tail-page reads. That points at the live-overlay
guard, not refold publication, as the next insert-path cost.

The branch-tail overlay cache lookup and store paths use the root active cache
owner so nested prepared executions inside one transaction can reuse verified
tail ranges. The post-insert cache advance path still receives the current
active statement. For nested prepared row executions, that can leave the
root-owned cache unadvanced after a successful maintained branch insert, so the
next row scans the suffix appended by the previous row.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This is first-party MyLite
  storage work only.
- MariaDB insert execution reaches MyLite through
  `mariadb/sql/handler.cc::handler::ha_write_row()` and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `packages/mylite-storage/src/storage.c::index_branch_tail_has_live_overlay()`
  stores and looks up branch-tail overlay cache entries through
  `branch_tail_overlay_cache_statement_for_file()`, which resolves the root
  active cache owner.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  advances branch-tail overlay cache entries after a successful maintained
  branch insert by calling
  `advance_branch_tail_overlay_caches_after_branch_insert()` with the current
  active file statement.
- `active_cache_statement_from_statement()` is the established helper for
  resolving root-owned active caches across nested statements.

## Design

- Resolve the cache owner inside
  `advance_branch_tail_overlay_caches_after_branch_insert()` before iterating
  cache entries.
- Keep the existing safety gates: advance only absent-overlay entries, only
  indexes whose maintained branch plan suppressed append-tail index-entry page
  publication, and only to a larger scanned page count.
- Keep present-overlay entries conservative.
- Add test-hook coverage proving a nested statement advances a parent-owned
  branch-tail overlay cache entry and does not create a child-owned entry.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or file-format behavior changes.
The slice only extends existing transient cache advancement to the same owner
used by cache lookup and storage.

## Single-File And Lifecycle Impact

No durable bytes, journal format, recovery behavior, or companion-file behavior
change. Branch-tail overlay cache entries remain active statement scratch state
and are still cleared by rollback and statement cleanup.

## Binary Size And Dependencies

No dependency changes. Binary-size impact is limited to a small owner-resolution
call and one internal test hook.

## Tests And Verification Plan

- Add a storage test hook proving nested branch-tail cache advancement updates
  the root-owned absent cache.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

Verification results:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  134.28 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  emitted branch-tail overlay counters of `2` scans and `48` scan reads,
  compared with the prior counter evidence of `65` scans and `1407` scan reads.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in 186.23 seconds.

## Acceptance Criteria

- Nested maintained branch inserts advance root-owned absent branch-tail overlay
  cache coverage.
- Child statements do not accumulate separate branch-tail overlay cache entries
  for that advance.
- Existing live overlay detection remains conservative for row-state and
  index-entry pages.
- Storage tests, embedded storage-engine smoke tests, formatting checks, and
  the focused prepared-insert benchmark pass.

## Risks

The main risk is over-advancing a cache after a write that produced an overlay
page. The existing gate remains unchanged: only branch-plan entries whose
`index_entry_changed` flag was cleared are advanced, and inserts do not publish
row-state pages.
