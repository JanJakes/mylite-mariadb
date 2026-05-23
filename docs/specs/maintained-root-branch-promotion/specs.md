# Maintained Root Branch Promotion

## Problem

When a single-page maintained root reaches capacity, insert currently marks the
root with an overflow-tail flag and leaves the new index-entry page in the
append history. Readers then scan that tail for every rooted lookup until a
later delete happens to refill the root. This is correct, but it keeps the
first over-capacity insert on the slower append-tail path instead of moving the
index toward the planned navigable layout.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `plan_maintained_index_root_inserts()` already identifies full maintained
  roots and protects those root pages in the write journal.
- `write_maintained_index_root_overflow_flags()` records the first fallback
  index-entry page for overflow readers.
- `read_live_index_entries_from()` can fold a maintained root entryset plus
  append-tail index/state pages into live sorted entries.
- Branch roots now carry page-owned total entry counts and readers follow
  stored child page ids, so a root page can be rewritten into a branch without
  requiring a catalog rewrite.

## Scope

- On insert overflow for a maintained root, after fallback index-entry pages are
  written, collect the live root-plus-tail entries for that index.
- If those entries still exceed single-page root capacity but fit in one
  branch-rooted leaf run, append leaf pages and rewrite the existing root page
  as a single-level branch root.
- Advance the unpublished header page count to cover the appended branch leaf
  pages before publishing.
- Leave later insert/update/delete maintenance for branch roots on the existing
  append-tail overlay path.

## Non-Goals

- No branch-root insert/update/delete in-place maintenance.
- No branch split, merge, rebalance, or delete compaction.
- No multi-level branch tree.
- No promotion for existing overflow roots outside the insert path.
- No catalog rewrite in this slice.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes the internal root page type selected after a maintained root overflows.

## Single-File And Lifecycle Impact

All promoted branch and leaf pages are durable pages in the primary `.mylite`
file. Old overflow-tail pages become orphaned like other superseded append-only
storage pages until compaction exists. No companion files are introduced.

## Recovery Impact

The existing preplanned dirty-page journal already protects the full maintained
root before insert overflow writes it. Appended branch leaf pages are ignored
unless the final header publish succeeds. Recovery coverage must verify stale
statement and transaction journals restore the pre-promotion root bytes and
logical visibility.

## Test Plan

- Extend maintained-root overflow storage coverage to expect the root page to
  become a branch page after the over-capacity insert.
- Verify exact lookup, full index reads, and row materialization see all
  entries through the promoted branch root.
- Verify a later insert after promotion is visible through the branch append
  tail.
- Add rollback/recovery coverage for promotion inside ordinary statement
  rollback plus stale statement and transaction journals.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

## Implementation Notes

- Promotion runs after fallback index-entry pages are written and before the
  header is published, using an unpublished header view that includes the new
  tail pages.
- The existing root page id is reused for the branch page, so catalog
  index-root metadata does not need to be rewritten.
- Appended branch leaf pages are ignored unless the final header publish
  succeeds; the already-protected root page is restored by statement or
  transaction recovery when promotion is abandoned.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Acceptance Criteria

- Insert overflow of a maintained root promotes that root to a single-level
  branch page when the live entries fit in one branch.
- The root page id stays stable and no catalog rewrite is required.
- Branch reads include entries folded from the former overflow tail.
- Future row DML after promotion remains visible through the append-tail
  overlay.
- Recovery tests cover the dirty-root rewrite.

## Risks And Follow-Ups

- This intentionally creates a static branch snapshot, not a maintained branch
  tree. Future row DML after promotion still uses append-tail overlays.
- Repeated post-promotion writes can grow a new tail until later branch DML or
  compaction slices exist.
