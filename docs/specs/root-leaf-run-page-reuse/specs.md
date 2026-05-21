# Root Leaf Run Page Reuse

## Problem

Published index leaf reads validate and decode the root leaf page before they
can derive the contiguous run metadata. The exact-match and full-run helpers
then ask for page offset `0` through `read_index_leaf_run_page()`, which goes
back through the durable leaf-page cache even though the caller already owns the
validated root page bytes and decoded metadata.

For single-page published roots, this leaves redundant cache lookup, page copy,
and decoded-struct reconstruction on every exact leaf probe.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- No MariaDB source change is required. The affected paths are first-party
  MyLite storage helpers under `packages/mylite-storage/src/storage.c`.
- `read_index_leaf_run_root()` reads the root page, verifies table id and index
  number, derives `mylite_storage_index_leaf_run`, and returns both the page
  bytes and decoded `mylite_storage_index_leaf_page`.
- `append_index_leaf_run_matches_to_row_id_list()`,
  `append_index_leaf_run_matches_to_entryset()`,
  `find_index_leaf_run_first_match_row_id()`,
  `append_index_leaf_run_entries_to_entryset()`, and
  `find_index_leaf_run_match_page()` may request page offset `0` immediately
  after that root validation.
- `read_index_leaf_run_page()` remains the correct path for non-root offsets
  because those pages were not decoded by the run-root lookup.

## Design

Thread the already validated root page through leaf-run readers:

- keep `read_index_leaf_run_root()` as the validation and run-derivation point;
- add an internal `read_index_leaf_run_page_or_root()` helper that returns a
  copy of the caller's validated root page for offset `0`;
- keep `read_index_leaf_run_page()` for all non-root offsets;
- use the root-aware helper for binary-search probes, duplicate-neighbor walks,
  exact match reads, and full-run reads; and
- leave append-tail overlay scanning unchanged.

## Non-Goals

- No public API change.
- No file-format change.
- No change to leaf run publication, ordering, duplicate-key semantics, or
  append-tail visibility overlays.
- No maintained B-tree or pager work.

## Compatibility Impact

SQL-visible behavior should not change. The same leaf page bytes are used; the
storage layer simply reuses the page already validated by the root lookup.

## Single-File And Lifecycle Impact

The slice only changes transient read control flow over existing primary-file
pages. It adds no durable state and no companion files.

## Tests

Existing storage tests cover single-page and multi-page leaf roots, duplicate
boundaries across leaf pages, full index reads from leaf runs, append-tail
overlays, and static first-row exact probes. Keep those passing.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

## Acceptance Criteria

- Leaf-run helpers use the decoded root page for page offset `0`.
- Non-root leaf pages still use normal page validation.
- Existing leaf-run exact and full-read storage tests pass.
