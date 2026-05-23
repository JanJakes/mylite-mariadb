# Branch Root Entry Counts

## Problem

Single-level branch roots currently rely on the catalog index-root record for
the total entry count. That is fine for copy-rebuilt roots, because the rebuild
publishes a fresh catalog record. It is a poor fit for future maintained-root
splits, where the stable root page should be rewritten into a branch page while
the catalog root page id stays unchanged.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c` and
  `packages/mylite-storage/src/storage_format.h`; no upstream MariaDB source
  changes are needed.
- `TABLE_INDEX_BRANCH` pages already reserve header bytes between the level
  field and payload offset.
- `read_index_branch_leaf_run_root()` uses `root_entry->definition_size` to
  derive leaf page counts and last-page entry counts.
- `mylite_storage_read_index_root()` reports maintained root counts from the
  root page, but branch roots still report the catalog count.

## Scope

- Add a durable branch-page total entry-count field in the existing reserved
  branch header space.
- Encode new branch roots with the total live entry count.
- Decode old branch pages with a zero entry count as legacy catalog-count
  pages.
- Make branch-root readers prefer the page entry count when it is present.
- Make index-root metadata report the page entry count for branch roots when it
  is present.

## Non-Goals

- No maintained-root split writer in this slice.
- No multi-level branch tree.
- No catalog rewrite changes.
- No SQL-visible behavior change.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. Existing branch
roots with a zero count remain readable by falling back to the catalog index-root
record count.

## Single-File And Lifecycle Impact

The new count is stored in the primary `.mylite` branch page. No companion files
or lifecycle changes are introduced.

## File-Format Impact

`TABLE_INDEX_BRANCH` page type `13` starts using a previously reserved u64
header field for total entry count. Decoding accepts zero as a legacy value.

## Test Plan

- Extend branch-page encode/decode coverage to verify the count field.
- Corrupt the count below child count and expect decode rejection.
- Verify rebuilt branch-root metadata reports the branch page's entry count.
- Keep branch lookup and noncontiguous child-id coverage passing.
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

- Branch pages now store total entry count at
  `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET`.
- `decode_index_branch_page()` accepts zero as a legacy count and rejects
  nonzero counts smaller than the child count.
- Branch-rooted leaf-run reads use the page count when present, falling back to
  the catalog count only for legacy zero-count branch pages.
- Index-root metadata reads also report a branch page-owned count when present.

## Verification Results

Passed on 2026-05-23:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

## Acceptance Criteria

- New branch roots persist and decode a total entry count.
- Legacy zero-count branch pages keep using the catalog count.
- Branch-root reads and metadata reads use the page count when present.
- The roadmap still marks the maintained-root split writer as pending.

## Risks And Follow-Ups

- This is a file-format extension, but it uses reserved zeroed branch header
  space and preserves legacy zero-count decode behavior.
- Future maintained-root split work must still add journal-protected root
  conversion, appended leaf pages, recovery coverage, and root-delete/merge
  policy.
