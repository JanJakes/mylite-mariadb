# Branch Root Tail Refold Inserts

## Problem

Final-leaf branch splits must not move a branch root's tail past existing
row-state or append-tail index-entry pages, because those pages still define
live visibility over the branch snapshot. The safe split path therefore falls
back as soon as the branch tail contains live overlay. Append-heavy workloads
that also have recent deletes, updates, or fallback entries should still be
able to publish a clean branch snapshot when the refolded live entries fit in a
single-level branch root.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `read_index_leaf_entries()` already combines branch-rooted leaf entries with
  append-tail row-state and index-entry overlay.
- `prepare_index_branch_snapshot_pages()` and
  `write_index_branch_snapshot_pages()` already build and publish a fresh
  single-level branch root plus contiguous leaf pages for a fixed-width
  entryset.
- `begin_write_journal_for_statement_pages()` can protect existing dirty pages;
  a refold needs to protect the branch root while newly appended leaf pages
  remain unpublished until the header page count advances.

## Scope

- When a full final child leaf cannot split because the existing branch tail
  has live overlay, read the current live index entryset, append the new row's
  index entry, and check whether the result still fits in one branch page.
- If it fits, suppress the fallback index-entry page, append fresh leaf pages,
  and rewrite the existing branch root to point at the new leaf run.
- Leave old branch child leaf pages as unreachable historical pages; file
  shrinking and free-list reuse remain separate compaction work.
- Keep fallback behavior when the refolded entryset does not fit in one branch
  page or when the root/key shape is unsupported.

## Non-Goals

- No multi-level branch roots.
- No interior page split or redistribution.
- No free-list reclamation of superseded branch leaves.
- No update/delete branch maintenance.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes the internal publication path for supported fixed-width index inserts.

## Single-File And Lifecycle Impact

The branch root is an existing dirty page protected by the rollback journal.
Fresh refolded leaf pages are appended past the current header page count and
become durable only when the header is published. Rollback and stale recovery
must restore the old branch root and truncate unpublished refold pages.

## Recovery Impact

Recovery coverage must prove that stale statement and transaction journals
restore the pre-refold branch root and do not leave the appended refold leaves
visible.

## Test Plan

- Extend branch-root storage coverage so an existing same-table tail overlay
  refolds into a fresh branch snapshot instead of writing a fallback index-entry
  page.
- Verify page count, root entry count, child count, child page ids, exact
  lookup, row materialization, full index reads, and later delete visibility.
- Cover statement rollback, stale statement recovery, and stale transaction
  recovery for the refold path.
- Run the same storage and storage-smoke verification set as the preceding
  branch split slice.

## Acceptance Criteria

- A supported insert into a branch root with live tail overlay can rewrite the
  root to a fresh single-level branch snapshot when the refolded live entryset
  fits.
- Existing tail overlay is incorporated into the new branch snapshot rather
  than hidden behind a moved branch tail.
- Fallback remains in place for oversized or unsupported refolds.
- Docs and roadmap distinguish this from general B-tree split/merge work.

## Risks And Follow-Ups

- This can grow the file by leaving superseded leaf pages unreachable. Free-list
  reuse for old index leaves belongs to a later compaction/reclamation slice.
- This remains a single-level branch maintenance step, not production B-tree
  navigation.

## Implementation Notes

- Branch insert planning now has a refold plan type separate from in-place leaf
  inserts and final-leaf splits.
- When the existing branch tail has live overlay, planning reads the current
  leaf-root entryset with the overlay applied, appends the candidate entry, and
  suppresses the fallback only if the refolded entryset still fits in one
  single-level branch root.
- The refold plan owns that planning-built entryset until execution, so the
  writer can prepare the replacement branch snapshot without reading and
  appending the same branch root entries again.
- The writer rebuilds a fresh branch snapshot at the existing root page id and
  appends new leaf pages after the row and any unrelated fallback pages.
- The old branch child leaves are left unreachable; reclaiming those pages is
  deferred to a compaction/free-list slice.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
