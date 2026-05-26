# Branch Refold Plan Entryset Reuse

## Problem

Live-overlay branch inserts can only rewrite a single-level branch root after
they prove that the current live entryset plus the inserted entry still fits in
one branch snapshot. Planning already builds that live entryset. Execution then
re-reads the same branch root, decodes the same leaves, appends the same entry,
and only then writes the refreshed snapshot. Prepared insert profiling shows
that duplicate branch entryset materialization remains part of the hot path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:mysql_insert()` drives prepared and direct
    inserts through the server execution pipeline.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` dispatches row writes to
    the selected handler.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` maps MariaDB
    row and key images into `mylite_storage_append_row_with_index_entries()`.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    calls the live-overlay refold fit check before suppressing the fallback
    index-entry page.
  - `packages/mylite-storage/src/storage.c:build_branch_index_refold_insert_entryset_if_fit()`
    reads the live leaf-root entryset, appends the candidate entry, and checks
    branch capacity.
  - `packages/mylite-storage/src/storage.c:refold_branch_index_root_insert()`
    currently repeats the branch-root entry read before preparing and writing
    the snapshot pages.

## Scope

- Reuse the planning-built refold entryset for the matching planned branch
  insert.
- Keep the existing execution-time read fallback for callers that do not carry
  a planned entryset.
- Preserve the existing journal protection, page-writing order, rollback,
  transaction recovery, and append-tail visibility rules.

## Non-Goals

- No cross-row or cross-statement branch-root entryset cache.
- No new durable page format, catalog field, public C API, or storage-engine
  routing behavior.
- No broader B-tree split, merge, or mutable-page design.
- No change to live-overlay eligibility rules.

## Design

The refold fit helper becomes a builder that returns the sorted entryset when
the refold fits. Planning transfers that entryset into the branch insert plan.
When execution sees a refold plan with an owned entryset, it prepares the branch
snapshot directly from that entryset instead of calling
`read_branch_index_root_entries()` and appending the same row again. Execution
only uses that entryset when the planned row id still matches the actual row id;
otherwise it falls back to the old branch-root read path.

The plan owns the entryset until `clear_maintained_index_insert_plan()`, so all
normal error paths, fallback paths, and statement cleanup paths release the
transient buffers. If a refold plan cannot be recorded because the statement
journal budget is exhausted, the builder frees the entryset and the existing
append-tail fallback remains available.

## Compatibility Impact

No SQL, public API, metadata, file-format, or engine-routing behavior changes.
MariaDB still observes the same row visibility, duplicate-key ordering, and
statement rollback behavior.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The reused entryset is
process-local planning state. The branch root is still journal-protected before
rewrite, and freshly appended leaf pages still become visible only when the
header page count is published.

## Build, Size, And Dependencies

No dependency or license impact. Binary-size impact is limited to a small
first-party plan-owned entryset field and cleanup helper.

## Test Plan

- Extend storage tests for a live-overlay branch refold to assert execution does
  not re-read the branch root entryset after planning.
- Keep existing refold visibility, rollback, transaction recovery, and lookup
  assertions.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Planned live-overlay branch refolds prepare snapshot pages from the
  planning-built entryset.
- Refold execution keeps the existing full read fallback for unplanned callers.
- Tests prove that the live-overlay refold path no longer calls
  `read_branch_index_root_entries()` during execution.
- Rollback and recovery coverage for the existing refold path still passes.

## Verification Results

Local environment: macOS worktree, `dev` and `storage-smoke-dev` presets.

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `160.85 sec`.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure`: passed in
  `35.52 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000`: prepared insert step
  component `85.836 us/op`, prepared insert commit component `8.436 ms`.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`: passed.

## Risks And Follow-Ups

- Holding a full branch entryset in the per-row plan adds transient memory
  pressure for refolded inserts. That is bounded by the same one-branch fit
  check that already limits the refold path.
- This only removes duplicate materialization inside one row insert. A broader
  active-statement branch-refold cache may still be needed if profiling shows
  repeated planning reads dominate after this slice.
