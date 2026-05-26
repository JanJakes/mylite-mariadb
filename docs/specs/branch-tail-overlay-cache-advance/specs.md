# Branch Tail Overlay Cache Advance

## Problem

The active branch-tail overlay cache avoids duplicate scans inside one branch
insert planning decision, but the next insert still scans pages appended by the
previous successful maintained branch insert. Those pages are row payload,
maintained leaf, and branch/root rewrite pages, not live index-entry or
row-state overlay pages for the maintained index. A fresh local
`prepared-insert-components` sample still showed
`index_branch_tail_has_live_overlay()` reading the same one-operation suffix on
the next prepared insert step.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB reaches the MyLite handler through the normal handler mutation
  wrappers in `mariadb/sql/handler.cc`.
- MyLite durable inserts flow through
  `mariadb/storage/mylite/ha_mylite.cc` into
  `mylite_storage_append_row_with_index_entries()`.
- In `packages/mylite-storage/src/storage.c`,
  `plan_maintained_index_root_inserts()` marks a maintained branch insert by
  clearing `index_entry_changed[entry_index]`, which suppresses fallback
  append-tail index-entry page publication for that index.
- The same insert path writes row payload pages, maintained branch/leaf pages,
  optional fallback index-entry pages for other changed indexes, then publishes
  the final header page count.

## Design

After a successful insert has published the final page count, advance active
branch-tail overlay cache entries for indexes that were maintained by the
branch insert plan:

- only update caches on an active storage statement;
- only advance indexes whose branch or deep-branch plan entry cleared
  `index_entry_changed[entry_index]`;
- advance absent cache entries for the same table id and index number to the
  final page count;
- leave present-overlay cache entries untouched;
- do not create a new cache entry after the fact, because the planning scan is
  the proof source for that branch tail.

Fallback index-entry pages for other indexes do not invalidate the advanced
cache because the branch-tail overlay check matches the same table and same
index number for index-entry pages. Inserts do not publish row-state pages.

## Scope

- Advance active branch-tail overlay caches after successful maintained branch
  inserts.
- Add regression coverage with two full-leaf maintained inserts inside the
  same active statement; the second insert must not perform another tail scan.
- Record benchmark evidence for prepared insert components.

## Non-Goals

- No durable branch-tail cache.
- No update/delete cache advance; row-state pages make those paths different.
- No change to fallback append-tail semantics.
- No new file-format or public API behavior.

## Compatibility Impact

No SQL-visible behavior changes. This only removes redundant storage planning
reads after MyLite has already chosen the same maintained-branch path.

## Single-File And Embedded Lifecycle

The cache advance is statement-local and disappears on rollback, commit close,
or statement cleanup. It adds no files and changes no recovery behavior.

## Public API, Storage Routing, Binary Size, And Dependencies

No public API, routing, file-format, or dependency impact. Binary-size impact is
limited to a small first-party helper.

## Tests And Verification Plan

- Extend branch leaf split coverage to execute two maintained full-leaf inserts
  in one active statement and assert that the branch-tail scan count remains at
  one.
- Run:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 155.14 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed in 32.06 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  reported prepared insert step at 99.963 us/op.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- Maintained branch inserts advance active absent overlay cache coverage to the
  final page count.
- Present-overlay cache entries remain conservative and are not rewritten as
  absent.
- Repeated full-leaf maintained inserts in one active statement avoid a second
  branch-tail scan.
- Storage and storage-smoke tests pass.
- Prepared insert component timing improves or at least does not regress
  locally.

## Risks

- Advancing a cache for an index that later writes an append-tail index-entry
  page would be incorrect. The implementation must therefore use the maintained
  insert plan's `index_entry_changed` state as the gate.
- Future update/delete support must not reuse this helper directly, because
  those paths can publish row-state pages that are table-wide overlay barriers.
