# Branch Tail Present Overlay Publication Cache

## Problem

The prepared-insert component benchmark still shows a small number of
branch-tail overlay reads after root-owned cache advancement and active leaf
page cache tuning. A local diagnostic run for
`mylite_perf_baseline --phase=prepared-insert-components 1000 100000` showed
the largest remaining tail reads from branch checks that rediscovered live
append-tail index-entry pages already published by earlier fallback writes in
the same active transaction.

The existing branch-tail cache deliberately keys normal matches by branch level
because absent no-overlay coverage is a statement about a scanned tail range for
that branch shape. Present-overlay entries are different: a concrete row-state
or index-entry page after the current branch's maximum child page is enough to
prove that the branch cannot hide the live overlay. Insert publication already
knows the page ids of fallback index-entry pages, so planning should not need
to rediscover those concrete barriers by scanning.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice only changes
  first-party MyLite storage planning.
- MariaDB insert execution reaches MyLite through
  `mariadb/sql/handler.cc::handler::ha_write_row()` and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `packages/mylite-storage/src/storage.c::index_branch_tail_has_live_overlay()`
  scans pages after a branch's maximum child page id and treats a matching
  append-tail index-entry page or table row-state page as a live overlay.
- `packages/mylite-storage/src/storage.c::write_index_entry_pages()` and the
  inline insert paths publish fallback index-entry pages for indexes whose
  maintained branch plan leaves `index_entry_changed` set.
- `packages/mylite-storage/src/storage.c::find_branch_tail_overlay_cache()`
  requires matching branch level for both absent and present cache entries.

## Design

- Keep the existing same-level cache lookup for absent coverage and same-level
  present-overlay hits.
- After a successful insert publishes its final header, store present-overlay
  cache entries for every fallback index-entry page that was actually written.
- Store these entries on the root active cache owner and key them by table id,
  index number, key size, and concrete overlay page id.
- Before scanning, search the active statement's branch-tail cache for any
  present-overlay entry with the same table id, index number, and key size whose
  overlay page id is still greater than the current branch's maximum child page
  id.
- Reuse only present-overlay entries across branch levels. Do not reuse absent
  no-overlay cache entries across levels.
- Leave cache storage, eviction, owner resolution, rollback clearing, and
  post-insert advancement unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or file-format behavior changes.
The change is a conservative planning-cache reuse for a barrier page that the
existing code already treats as live overlay when found by scanning.

## Single-File And Lifecycle Impact

The cache remains active-statement scratch state. No primary-file bytes,
journal format, recovery behavior, or companion-file lifecycle changes.

## Binary Size And Dependencies

No dependency change. Binary-size impact is limited to small cache-store/search
helpers and a focused storage test hook.

## Tests And Verification Plan

- Add a storage test hook proving a level-`2` branch check reuses a present
  overlay cached from a published fallback index-entry page without performing
  a scan.
- Keep key-size matching explicit so differently shaped index cache entries do
  not prove overlay presence for this branch.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Present-overlay cache entries can be reused across branch levels only when
  table id, index number, key size, and overlay page position prove the same
  live-overlay barrier.
- Absent no-overlay cache entries remain level-shaped.
- Existing live overlay detection remains conservative for row-state and
  index-entry pages.
- Storage tests, embedded storage-engine smoke tests, formatting checks, and
  the focused prepared-insert benchmark pass.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  170.44 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in 226.45 seconds.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported branch-tail overlay scans/read counts of `2` / `48`, down from
  `36` / `285` immediately before this slice on the same benchmark shape.

## Risks

The main risk is broadening cache reuse too far. The implementation avoids
reusing absent coverage across levels and keeps key-size matching for present
entries, so it only skips a scan when a concrete cached overlay page is still
after the current branch's maximum child page.
