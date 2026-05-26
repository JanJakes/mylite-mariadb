# Branch Snapshot Direct Leaf Buffer

## Problem Statement

Branch-root refold and branch leaf rebuilds prepare leaf pages by first
allocating a leaf-only page run, then allocating the final branch-plus-leaf
buffer and copying the leaf pages into it. The prepared-insert profile now
spends most time in branch snapshot preparation, leaf encoding, and checksums;
the extra temporary leaf buffer and page-run copy do not change behavior and
add avoidable allocation and memory traffic.

This slice lets branch snapshot preparation encode leaf pages directly into
the final branch-plus-leaf buffer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are involved.
- `prepare_index_branch_snapshot_pages()` currently calls
  `prepare_index_leaf_pages()`, allocates another buffer with one extra page
  for the branch root, and copies the leaf pages after that branch page.
- `encode_index_leaf_page()` already writes a single page into caller-provided
  storage, so the multi-page leaf encoder can be factored from
  `prepare_index_leaf_pages()` and reused by branch snapshot preparation.

## Design

- Factor leaf page layout calculation into a helper shared by standalone leaf
  preparation and branch snapshot preparation.
- Factor multi-page leaf encoding into a helper that writes into a
  caller-provided buffer.
- Keep `prepare_index_leaf_pages()` behavior unchanged for callers that need a
  standalone leaf-page allocation.
- Make `prepare_index_branch_snapshot_pages()` allocate only the final
  branch-plus-leaf page run, then encode leaf pages directly at offset one.
- Derive branch child fences from the final leaf-page buffer.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, file-format, or
wire-protocol behavior changes. The encoded branch and leaf pages are intended
to be byte-equivalent for the same input entryset.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, locking, or companion-file changes.
This changes only transient page-preparation memory ownership before pages are
written into the primary `.mylite` file.

## Binary Size And Dependencies

No dependency changes. The code-size impact is limited to small helper
factoring and an internal test hook.

## Test And Verification Plan

- Add an internal branch snapshot layout test that builds a two-leaf branch
  snapshot and validates branch child ids, child max keys/row ids, and leaf
  entry counts.
- Run the first-party storage target and storage CTest selection.
- Run formatting checks, storage-smoke embedded storage-engine coverage, and
  prepared-insert component performance baselines.

## Acceptance Criteria

- Branch snapshot preparation no longer allocates or copies a separate
  leaf-only page run.
- Standalone `prepare_index_leaf_pages()` behavior remains covered.
- Branch snapshot output preserves branch child fences and leaf page layout.
- Targeted tests and checks pass.

## Risks

- This is a mechanical buffer ownership refactor in a hot storage path. The
  primary risk is an off-by-one page offset in the final branch-plus-leaf
  buffer, covered by the new branch snapshot layout regression and existing
  branch storage tests.
