# Branch Left Range Planning Buffer

## Problem

A fresh local sample after the active leaf-cache metadata probe still shows
maintained-index insert planning under
`try_plan_branch_leaf_range_insert_redistribution_left()`. The leftward
candidate expansion currently prepends each newly scanned sibling leaf id by
moving the already planned id array one slot to the right. The array is small,
but this runs on the prepared-insert hot path and shows up as `_platform_memmove`
under left-range planning.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts still reach MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`;
  - `mariadb/sql/handler.cc:handler::ha_write_row()`;
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`.
- `packages/mylite-storage/src/storage.c::try_plan_branch_leaf_range_insert_redistribution_left()`
  scans left sibling leaves in descending branch-child order, but the planned
  range must be passed to the writer in ascending branch-child order.
- The current implementation preserves ascending order by prepending into a
  fixed local array with `memmove()`.

## Design

Reserve the selected leaf near the end of the fixed local page-id buffer for
leftward planning. Each newly scanned left sibling can then be written into the
previous slot without moving existing ids. The planner passes the occupied
buffer slice to `append_branch_leaf_range_insert_redistribution_if_fit()` so the
writer still receives the same ascending leaf-id order and the same
`range_start_child_index`.

## Non-Goals

- No redistribution policy, page format, journal budget, or writer change.
- No branch right-range planning change; it already appends without moving ids.
- No SQL-visible behavior, public API, storage-engine routing, or file-format
  change.

## Compatibility And Lifecycle Impact

No MySQL/MariaDB compatibility change. The same sibling leaves are scanned, the
same capacity checks are applied, and the same rollback-protected writer plan
is produced. Durable file, journal, lock, and companion-file behavior are
unchanged.

## Test And Verification Plan

- Keep existing branch sibling/range redistribution tests passing, including
  rollback and level-two branch range cases.
- Run:
  - `git diff --check`;
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`;
  - `cmake --build --preset dev --target mylite_storage_test`;
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`;
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`;
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`;
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`.

## Acceptance Criteria

- Left-range planning no longer uses `memmove()` to prepend candidate leaf ids.
- Planned leaf id order remains ascending and range start indexes are unchanged.
- Existing branch redistribution and routed storage tests pass.

## Verification Results

The final local sample showed
`try_plan_branch_leaf_range_insert_redistribution_left()` spending time in
summary leaf scans and fit checks, with no nested `_platform_memmove` under the
left-range planner. Other unrelated page-copy and append-buffer copies remain
elsewhere in the prepared-insert profile.

The committed-tree 100k prepared-insert component run reported `28.864 us/op`
for the prepared insert step, `19.987 ms` for the prepared insert commit,
`31,653,888` final bytes, and `7,728` header pages. The timing was noisy, but
the file shape stayed unchanged.

## Risks

- Off-by-one mistakes would mis-order planned leaf ranges. The implementation
  keeps the occupied slice explicit and relies on existing redistribution tests
  to validate the resulting index contents.
