# Branch Leaf Refresh Offset

## Problem

The current prepared-insert profile attributes dirty-page pressure mostly to
`insert_branch_index_leaf_entry`, with `54,289` dirty `index-leaf` pressure
admissions and `105` dirty `index-branch` admissions in the latest
storage-smoke benchmark. That writer receives a branch insert plan produced by
`find_index_branch_child_page_for_insert_and_offset()`, but the plan keeps only
the selected leaf page id. During execution,
`refresh_index_branch_child_after_leaf_insert()` scans the branch payload again
to find the same child cell before updating its fence and entry count.

The redundant scan is small per row but sits in the hot maintained-branch
insert path. The next slice should reuse the child offset already established
during planning without changing branch page bytes or durability behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB-facing routed inserts enter MyLite storage through
  `mariadb/storage/mylite/ha_mylite.cc`; this slice does not change handler
  routing or SQL semantics.
- MyLite storage plans single-level branch inserts in
  `packages/mylite-storage/src/storage.c` by calling
  `find_index_branch_child_page_for_insert_and_offset()`, which returns both
  the child leaf page id and child offset.
- `mylite_storage_branch_index_insert` currently stores the selected
  `leaf_page_id` but not the selected child offset.
- `insert_branch_index_leaf_entry()` later calls
  `refresh_index_branch_child_after_leaf_insert()`.
- `refresh_index_branch_child_after_leaf_insert()` scans branch child cells by
  page id before updating the child fence, total branch entry count, and
  checksum-dirty state.

## Design

- Add a transient `leaf_child_index` field to
  `mylite_storage_branch_index_insert`, using `SIZE_MAX` when an offset was
  not captured.
- Store the planning-time child offset for single-level branch inserts and for
  level-two inserts where the lower child branch offset is already known.
- Add an offset-aware branch child refresh helper. When a valid offset is
  present, validate that the child cell still references the expected leaf page
  and update it directly. If no offset is present, keep the existing scan path.
- Keep the existing corruption checks: stale or out-of-range offsets return
  `MYLITE_STORAGE_CORRUPT` instead of silently updating the wrong cell.
- Add test-hook counters for offset refresh hits and fallback scans, and print
  them in the prepared-insert storage counter table.

This slice is a control-plane hot-path optimization. It does not change branch
page layout, entry ordering, write ordering, dirty-page buffering, journaling,
or recovery.

## Affected Subsystems

- MyLite storage maintained-branch insert planning and execution.
- MyLite storage test hooks.
- Prepared-insert benchmark output.

No MariaDB SQL-layer, handler registration, or public `libmylite` API changes
are planned.

## Compatibility Impact

No SQL-visible behavior, storage-engine routing behavior, metadata behavior, or
public API behavior changes. `ENGINE=InnoDB` continues routing through MyLite
storage under the existing handler path.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The change only reuses
transient planning metadata inside the active statement path. Journal,
rollback, dirty-buffer, lock, and file lifecycle behavior are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Binary-size
impact is limited to a small first-party storage control-flow change plus
test-hook counters in development builds.

## Test And Verification Plan

- Add storage test-hook coverage for the offset-aware refresh helper:
  - a correct offset updates the selected child without falling back to scan;
  - a stale offset returns corruption.
- Run the prepared-insert benchmark and verify non-zero offset refresh hits.
- Keep the existing compatibility and smoke tests passing:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Single-level maintained branch inserts reuse the planning-time leaf child
  offset when refreshing the branch child cell.
- Stale or invalid stored offsets fail closed.
- Prepared-insert benchmark output reports offset refresh hits for the branch
  insert path.
- Existing storage and routed embedded storage-engine tests pass.

## Risks

The offset is transient planning metadata and must never be trusted without
checking the expected child page id at execution. Deeper branch writers can
continue using the existing scan path until their planning offsets are threaded
through the corresponding write helpers.
