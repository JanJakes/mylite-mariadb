# Branch Tail Absent Cache Seed

## Problem

The branch-tail overlay scan breakdown shows the remaining prepared-insert
branch-tail reads are not discovering live overlays: the 100k component run
reported `2` scans, `48` reads, `46` row-page skips, `2` index-structure skips,
and zero index-entry scans, row-state scans, other skips, or overlay hits. The
existing post-insert cache advance only updates branch-tail overlay cache
entries that already exist, so the first later check for a newly maintained
branch must still scan the suffix once.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite insert execution reaches first-party storage through
  `mylite_storage_append_row_with_index_entries()`.
- `plan_maintained_index_root_inserts()` clears
  `index_entry_changed[entry_index]` when a branch/deep-branch insert plan
  maintains the index directly and suppresses fallback append-tail index-entry
  publication for that index.
- `write_branch_index_root_inserts()` writes and refreshes active branch page
  cache entries for maintained branch roots before
  `advance_branch_tail_overlay_caches_after_branch_insert()` runs.
- Inserts do not publish row-state pages, so a successful maintained branch
  insert can prove the just-published suffix has no live branch-tail overlay
  for the maintained index.

## Design

- Extend post-insert branch-tail overlay cache advancement so it can create an
  absent cache entry when no prior cache entry exists.
- Use the root active branch page cache as the proof source for branch shape,
  key size, level, and maximum child page id.
- Seed only for branch and deep-branch plan entries whose
  `index_entry_changed[entry_index]` is clear.
- Do not seed from disk reads; if the active branch cache entry is missing,
  keep the current behavior.
- Do not overwrite present-overlay cache entries.
- Check concrete present-overlay cache entries before accepting absent cache
  coverage so later fallback index-entry publication cannot be hidden by an
  earlier seeded absent range.
- Keep existing cache advancement for already-known absent entries.

## Compatibility Impact

No SQL, public API, file-format, storage-engine routing, or single-file
lifecycle behavior changes. The slice only seeds transient statement-owned
planning cache entries after a successful insert path has already proven that
the maintained index did not publish an append-tail entry.

## Scope And Implications

- Affected subsystem: first-party branch-tail overlay planning cache.
- DDL metadata routing, durable page formats, public API, and recovery:
  unchanged.
- Dependency, license, and binary-size impact: small first-party helper and
  focused test-hook coverage only.
- Risk: creating an absent cache entry would be unsafe if the same insert wrote
  a same-index append-tail index-entry page or row-state page. The gate remains
  the maintained insert plan's cleared `index_entry_changed` bit, and insert
  paths do not write row-state pages.

## Test And Verification Plan

- Add a storage test hook proving a nested child statement can seed a
  parent-owned absent branch-tail overlay cache from an active branch page
  cache when no prior branch-tail cache entry exists.
- Extend present-overlay cache coverage so a concrete overlay entry wins even
  when same-branch absent coverage is already cached.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Successful maintained branch inserts can seed an absent branch-tail overlay
  cache from the active branch root cache.
- Existing absent cache entries still advance to the final published page
  count.
- Present-overlay cache entries remain conservative.
- Prepared-insert branch-tail overlay scans drop toward zero for the measured
  workload while storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  reported no changes.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in `305.14s`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `323.01s`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported branch-tail overlay scans/read pages at `0` / `0`; packed
  tail-append missing-page blockers at `2`; prepared insert step at
  `300.809 us/op`; and final file size at `31,539,200` bytes / `7,700` pages
  on this VPS.
