# Deep Branch Active Page Cache

## Problem

The 100k prepared-insert component sample after branch-tail overlay cache owner
fixes no longer points at tail-overlay scanning as the primary storage-local
issue. Remaining branch-maintenance evidence includes selected branch/leaf page
reads after the tree grows beyond the small level-`2` shape.

The active index page caches already own the correct transient view for root
and level-`2` branch insert planning, and pager writes refresh those caches
after branch and leaf rewrites. Deeper branch insert planning still has local
read/decode paths that bypass the active caches for selected child branch pages
and selected leaf pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution remains unchanged:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()` executes a row
    insert through the handler.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` calls the engine
    `write_row()` implementation.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` routes
    durable inserts to MyLite storage.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    dispatches maintained branch inserts by root level.
  - `packages/mylite-storage/src/storage.c:plan_level_three_branch_index_root_insert()`
    and `plan_level_four_branch_index_root_insert()` read selected descendant
    branch and leaf pages directly.
  - `packages/mylite-storage/src/storage.c:plan_deep_branch_index_root_insert()`
    already uses `read_index_branch_child_page()` for child branches, but that
    helper reads through the pager and decodes without first probing the active
    branch-page cache.
  - `packages/mylite-storage/src/storage.c:pager_write_page()` and
    `pager_write_buffered_maintained_index_page()` refresh active branch and
    leaf page caches after maintained index writes.

## Design

- Make the shared branch-child page reader probe the active branch-page cache
  before falling back to pager reads and decode validation.
- Populate the active branch-page cache after a branch-child reader miss and
  successful decode, so repeated same-statement deeper branch descent can reuse
  the decoded page.
- Add a selected branch-insert leaf reader that probes the active leaf-page
  cache before falling back to durable page reads and decode validation.
- Use that selected-leaf reader in single-level, level-`2`, level-`3`,
  level-`4`, and deeper branch insert planning.
- Keep the existing level-`2` leaf-plan counter semantics, but extend active
  branch-page plan reads to include branch-child reader misses when an active
  cache owner exists.

This deliberately does not add branch-cell child entry counts or a new durable
format. Avoiding first-time reads for sibling slack detection remains a separate
file-format decision.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The fallback path still uses the same checksum-validating decoders, and cached
pages are scoped to the active statement chain that already owns the current
checkpoint view.

## Single-File And Embedded Lifecycle

No companion files or durable layout changes. Active page caches remain
statement-local memory and are cleared by the existing rollback and catalog-root
invalidation paths.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. The slice adds a small selected-leaf reader
and extends the existing branch-child reader.

## Test And Verification Plan

- Add storage hook coverage proving the shared branch-child reader can satisfy
  a read from the active branch-page cache without touching the file.
- Keep existing active branch/leaf page cache coverage and level-`2` insert
  planning counter coverage passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `133.38 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `171.17 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed,
  with prepared insert step at `53.606 us/op`.

The 100k benchmark's current hot counters did not move: branch leaf-range plan
reads remained `1458`, branch refold entryset reads remained `471`, and
level-`2` branch leaf plan reads remained `218`. The next measured prepared
insert work should target branch leaf-range slack planning or branch-refold
entryset cache misses rather than deeper branch-page cache reuse.

## Acceptance Criteria

- Shared branch-child page reads reuse active cached branch pages when present.
- Branch insert planning for level-`3+` selected leaf pages can reuse active
  leaf-page cache entries.
- Cache misses retain existing durable validation and seed the active cache.
- Existing branch insert, split, rollback, and routed storage tests pass.
- The prepared insert component benchmark records the updated branch
  maintenance counters and timing.

## Risks

- Stale cached child branches would misroute deeper inserts. Existing pager
  write refresh and rollback invalidation rules are the correctness guard.
- The active cache is opportunistic and bounded. Eviction only loses the
  optimization because miss paths still reread and revalidate the page.
