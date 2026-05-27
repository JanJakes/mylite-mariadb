# Branch Refold Capacity Precheck

## Problem

The current 100k prepared-insert component sample still reports branch refold
entryset rebuilds after the active refold entryset cache work:

- branch refold entryset reads: `471`
- branch refold entryset cache hits: `669`

`build_branch_index_refold_insert_entryset_if_fit()` currently rebuilds or
copies the full branch logical entryset before checking whether the refolded
entry count can fit in one level-`1` branch root. For full or already-too-large
branch roots, the fit decision is derivable from the branch entry count,
fixed-key leaf capacity, and branch child capacity without reading child leaves.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution still reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    uses the refold fallback after local branch insert, split, and
    redistribution planning miss.
  - `packages/mylite-storage/src/storage.c:try_plan_branch_index_root_refold_insert()`
    delegates fit and entryset construction to
    `build_branch_index_refold_insert_entryset_if_fit()`.
  - `packages/mylite-storage/src/storage.c:build_branch_index_refold_insert_entryset_if_fit()`
    currently calls `copy_active_branch_refold_entryset_for_insert()` or
    `read_index_leaf_entries()` before computing leaf-page count and branch
    capacity.

## Design

Add an early fixed-width capacity precheck to branch refold planning:

1. Require the same level-`1` branch shape already accepted by the cache path.
2. Compute the post-insert entry count from `branch_page->entry_count + 1`.
3. Compute the required leaf page count from
   `index_leaf_entry_capacity(index_entry->key_size)`.
4. Compare that count with `index_branch_child_capacity(index_entry->key_size)`.
5. Return `fits = false` immediately when the refold cannot fit.

Only the no-fit path changes. Possible-fitting refolds still copy or rebuild
the full entryset and carry it into the writer as before.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The precheck skips work only when the later exact fit calculation would reject
the same refold capacity.

## Single-File And Embedded Lifecycle

No companion files or durable layout changes. The slice only avoids transient
in-process branch leaf reads during planning.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. Binary impact is limited to a small planning
branch and test-hook coverage.

## Test And Verification Plan

- Add storage hook coverage proving an over-capacity level-`1` branch refold
  returns no fit without incrementing the branch refold entryset read counter.
- Keep active refold entryset cache roundtrip coverage passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Over-capacity branch refold candidates skip entryset reads and cache copies.
- Possible-fitting candidates keep the existing entryset build and writer path.
- Existing branch insert, refold, rollback, and routed storage tests pass.
- The prepared insert component benchmark records updated refold counters and
  timing.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `158.74 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `191.26 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed,
  with prepared insert step at `54.192 us/op`.

The 100k sample did not hit this impossible-refold path: branch refold entryset
reads stayed at `471`, cache hits stayed at `669`, branch leaf-range plan reads
stayed at `1458`, and level-`2` branch leaf plan reads stayed at `218`. The
next measured insert work should target fitting refold reads or leaf-range
planning rather than no-fit refold capacity checks.

## Risks

- A wrong capacity formula would suppress a valid refold. The precheck uses the
  same fixed-width leaf capacity and branch child capacity helpers as the later
  fit calculation, and only returns early on the impossible case.
