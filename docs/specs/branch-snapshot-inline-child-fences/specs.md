# Branch Snapshot Inline Child Fences

## Problem Statement

Branch snapshot preparation encodes leaf pages and then walks the encoded leaf
pages to recover each child page's max key and row id for the branch root. The
leaf encoder already knows the ordered entry range written to each page, so the
second pass over encoded leaf cells is redundant work in the prepared-insert
branch-refold path.

This slice lets the multi-page leaf encoder optionally return each leaf page's
max row id and max key while it encodes the page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are involved.
- `prepare_index_branch_snapshot_pages()` calls
  `encode_zeroed_index_leaf_pages()` and then loops over the final leaf page
  buffer, reads each leaf entry count, locates the last encoded cell, and
  copies that cell's row id and key into child fence arrays.
- `encode_zeroed_index_leaf_pages()` already has the page's ordered entry
  window (`first_entry`, `page_entries`) and the optional raw-order mapping, so
  it can identify the final logical entry for each leaf without decoding bytes
  from the encoded page.

## Design

- Extend `encode_zeroed_index_leaf_pages()` with optional per-page max row-id
  and max-key output arrays.
- Keep standalone `prepare_index_leaf_pages()` passing `NULL` outputs so its
  behavior and allocation shape stay unchanged.
- Make branch snapshot preparation pass its child fence arrays into the encoder
  and remove the post-encode scan over leaf page payloads.
- Preserve the existing branch page encoder and branch snapshot layout.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, file-format, or
wire-protocol behavior changes. Branch and leaf pages should be byte-equivalent
for the same entryset.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, locking, or companion-file changes.
This changes only transient branch snapshot preparation before pages are written
into the primary `.mylite` file.

## Binary Size And Dependencies

No dependency changes. Code-size impact is limited to optional output arguments
on an internal helper.

## Test And Verification Plan

- Extend the branch snapshot layout regression to feed an out-of-order entryset
  while covering child page ids, child max keys/row ids, and leaf entry counts.
- Run the storage test target and CTest selection.
- Run formatting checks, storage-smoke embedded storage-engine coverage, and
  prepared-insert component performance baselines.

## Acceptance Criteria

- Branch snapshot preparation no longer scans encoded leaf pages to derive
  child fences.
- Standalone leaf page preparation remains unchanged.
- Branch snapshot branch cells still contain the expected max keys and row ids.
- Targeted tests and checks pass.

## Risks

- The main risk is mismatching the encoder's logical last entry with the branch
  page fence entry when the raw-order map is present. The branch snapshot
  layout regression now feeds an out-of-order entryset and checks the sorted
  child fences.
