# Branch Snapshot Root Dirty Buffer

## Problem Statement

Branch snapshot refolds write freshly generated leaf pages and then rewrite the
existing single-level branch root. The fresh leaves are append-only for the
statement, but the branch root is an existing maintained index page. Other
maintained root and branch rewrites already use the dirty-page buffer so active
statements can coalesce repeated root rewrites before commit.

This slice routes branch snapshot root rewrites through that same maintained
root/branch dirty-page buffer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are involved.
- `refold_branch_index_root_insert()` calls
  `prepare_index_branch_snapshot_pages()` and
  `write_index_branch_snapshot_pages()` for level-1 branch refolds.
- `write_index_branch_snapshot_pages()` writes generated leaf pages with
  `pager_write_prevalidated_index_leaf_page()`, then writes the existing branch
  root with `pager_write_page()`.
- Ordinary maintained root inserts already call
  `pager_write_buffered_maintained_root_or_branch_page()`, whose buffering path
  captures dirty-page undo, refreshes the active branch cache, and flushes at
  top-level statement commit.

## Design

- Keep branch snapshot leaf writes unchanged because they are freshly generated
  leaf pages and already use the prevalidated leaf writer.
- Write the branch snapshot root with
  `pager_write_buffered_maintained_root_or_branch_page()` instead of the generic
  pager write path.
- Preserve the fallback inside the buffered writer for calls outside an active
  writable statement, new pages, unsupported page sizes, or pages that are not
  maintained index roots/branches.
- Add a storage hook regression that prepares a branch snapshot against an
  existing page in an active statement, verifies the root rewrite is buffered,
  verifies the on-disk existing page is unchanged before rollback, and verifies
  rollback clears the buffered rewrite.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, wire-protocol, or
file-format behavior changes. Routed `ENGINE=InnoDB`, explicit MyLite, and
other maintained-index paths publish the same branch page bytes when the
statement commits.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Dirty-page undo and
rollback use the existing statement-owned recovery path. No new companion files,
file-format fields, or lock states are introduced.

## Binary Size And Dependencies

No dependency or license changes. Binary-size impact is limited to a focused
test hook and a narrower call to an existing pager helper.

## Test And Verification Plan

- Add storage hook coverage proving branch snapshot root writes use the
  dirty-page buffer for existing maintained branch pages.
- Keep existing branch snapshot layout and maintained-index rollback coverage
  passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `142.18 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`: passed
  in `33.64 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`: passed,
  with prepared insert step at `33.535 us/op`.

## Acceptance Criteria

- Branch snapshot root rewrites are eligible for the existing maintained
  root/branch dirty-page buffer.
- Generated leaf writes stay on the prevalidated leaf writer.
- Rollback preserves the existing on-disk page when a branch snapshot root
  rewrite was only buffered.
- Relevant tests, formatting checks, smoke tests, and the prepared insert
  component benchmark pass.

## Risks

- Buffering arbitrary pages would be unsafe. This slice relies on the existing
  helper's page-type guard, so only maintained index root or branch pages enter
  the dirty-page buffer.
- This reduces immediate write traffic, but it does not remove the larger CPU
  cost of rebuilding and checksumming branch snapshots. That remains the next
  performance target.
