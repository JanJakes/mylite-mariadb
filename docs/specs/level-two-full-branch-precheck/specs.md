# Level-Two Full-Branch Precheck

## Problem

The prepared-insert component baseline still reports repeated level-`2`
branch leaf planning reads after active branch and leaf cache work:

- level-two branch leaf plan reads: `203`
- branch tail overlay scans/read pages: `2` / `48`

`plan_level_two_branch_index_root_insert()` reads the selected descendant leaf
before deciding that a lower level-`1` branch is packed. For valid fixed-width
branch pages, a lower branch with `entry_count == child_count * leaf_capacity`
proves every child leaf is full, so the selected leaf read cannot change the
split decision.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_level_two_branch_index_root_insert()`
    descends from a level-`2` root to a lower branch and selected leaf.
  - `packages/mylite-storage/src/storage.c:read_index_branch_child_page()`
    validates child branch table, index, key size, and level before planning.
  - `packages/mylite-storage/src/storage.c:validate_index_branch_child_branch_fence()`
    validates the parent fence against the selected lower branch.

## Design

Use lower-branch metadata to skip the selected leaf read when the lower branch
is already known to be physically packed:

1. Compute fixed-width leaf capacity from the validated child branch key size.
2. Compute lower-branch capacity as `child_count * leaf_capacity`.
3. Treat `entry_count == capacity` as proof that every child leaf under that
   lower branch is full.
4. Plan the existing full-leaf split path directly without reading the selected
   leaf.

Non-full lower branches still read and validate the selected leaf exactly as
before. Live tail-overlay checks remain after the full-branch decision so the
planner still avoids hiding durable append-tail state behind maintained branch
rewrites.

## Compatibility Impact

No SQL, public API, storage-engine routing, metadata, or file-format behavior
changes. The slice only removes a redundant planning read from first-party
MyLite storage when already-validated branch metadata proves the selected
leaf's fullness.

## Single-File And Embedded Lifecycle

No companion files, recovery rules, or durable layout changes. The optimization
is transient in-process planning work and preserves the existing append-tail
fallback when live overlay pages are present.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. Binary impact is limited to a small planning
branch and storage test-hook coverage.

## Test And Verification Plan

- Add storage hook coverage building a valid level-`2` root and packed lower
  branch, then assert the planner produces a split plan without incrementing
  the level-two leaf-read counter.
- Keep existing level-`2` branch insert and storage smoke coverage passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Packed lower branches under level-`2` roots skip the selected leaf read during
  split planning.
- Non-packed lower branches keep the existing selected leaf validation path.
- Live tail-overlay safety checks still gate maintained branch rewrites.
- Existing storage and embedded storage-engine smoke tests pass.
- The prepared insert component benchmark records the updated level-two leaf
  planning counter.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `158.94 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `219.90 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed, with prepared insert step at `50.844 us/op`.

The measured prepared-insert component sample reduced level-two branch leaf
plan reads from `203` to `102`. Other current counters were branch leaf-range
plan reads `85`, branch refold entryset reads/cache hits `29` / `1111`, active
branch page plan reads `0`, branch insert writer branch/leaf decodes `0` / `0`,
and branch tail overlay scans/read pages `2` / `48`.

## Risks

- The precheck relies on branch entry counts staying exact. That is already a
  storage invariant for published branch pages; the test hook constructs the
  exact packed shape and existing decode/fence validation remains in the path.
  Broader corruption detection is unchanged for the non-packed and writer
  execution paths.
