# Direct Branch Exact Lookup

## Problem

Published branch roots can already select the child leaf that may contain an
exact key, but point lookup still enters through the generic leaf-run reader.
For branch roots that reader builds a transient list of every leaf page id and
reads the first leaf before the exact lookup descends to the target child. That
adds unnecessary work to hot primary-key and unique-key reads, and it makes an
unrelated first leaf page part of the point-lookup failure surface.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines the handler point-lookup contract through
  `index_read_map()` and later cursor calls. The storage engine owns physical
  index navigation.
- `mariadb/sql/handler.cc::handler::ha_index_read_map()` dispatches point
  lookup to the handler and normalizes status handling without prescribing
  storage layout.
- `mariadb/storage/mylite/ha_mylite.cc` resolves handler point reads through
  MyLite storage exact-index lookup helpers before row materialization.
- `packages/mylite-storage/src/storage.c::find_index_leaf_exact_static_row_id()`
  currently calls `read_index_leaf_run_root()` for every published root shape.
  For branch roots, `read_index_branch_leaf_run_root()` recursively collects
  every leaf page id and reads the first leaf before
  `find_index_leaf_run_first_match_row_id()` performs the actual exact-key
  branch descent.
- `packages/mylite-storage/src/storage.c::find_index_branch_leaf_page()`
  already performs recursive branch child selection using high `(key, row_id)`
  fences, so a point lookup can read only the target leaf when no append-tail
  overlay is present.

## Design

- Add a branch-root fast path inside `find_index_leaf_exact_static_row_id()`
  after the index-root catalog record is found.
- When the root page is a branch page:
  - validate root table, index, level, child count, and key size through the
    existing branch decoder,
  - walk branch pages to compute the highest page id in the static branch
    subtree while preserving internal-child metadata and high-fence validation,
  - if any page after the static subtree exists, leave `out_used_leaf` false so
    the existing overlay-aware exact-entry path handles appended row-state and
    index-entry pages,
  - otherwise use `find_index_branch_leaf_page()` to locate the one leaf that
    can contain the key, read that leaf, and binary-search it for the first
    matching row id.
- Decode non-branch maintained roots and contiguous leaf runs from the same
  root-page read, preserving their existing tail-overlay fallback without
  adding an extra root read to hot non-branch lookups.

## Non-Goals

- No exact-entryset streaming across adjacent branch leaves. Non-unique exact
  reads continue to use the existing leaf-run path.
- No prefix-read or full-index cursor rewrite.
- No new branch mutation support, branch sibling pointers, page compaction, or
  file-format change.

## Compatibility Impact

SQL and public API behavior do not change. This only reduces the internal
storage work for static no-tail exact point reads. MariaDB handler semantics
remain the compatibility boundary.

## Single-File And Lifecycle Impact

No file-format change and no new sidecar. The fast path reads existing branch
and leaf pages in the primary `.mylite` file and preserves the current
append-tail overlay fallback whenever static branch pages cannot answer alone.

## Public API, Storage Routing, And Wire Protocol

No public `libmylite` API, storage-engine routing, SQL policy, or wire-protocol
change.

## Binary Size And Dependencies

Two small first-party helper functions. No dependency and no meaningful binary
size impact.

## Tests And Verification

- Extend branch navigation storage tests so a point lookup in a later branch
  leaf succeeds after corrupting an unrelated first leaf checksum. This proves
  exact point lookup no longer reads the first leaf before the target leaf.
- Keep prefix and full-index readers on existing coverage, which still validates
  the generic leaf-run path.
- Run storage tests, storage smoke, whitespace checks, and clang-format diff
  checks for touched C files.

## Acceptance Criteria

- Static no-tail branch point lookup reads the selected branch leaf directly and
  returns the same row id as before.
- If a branch root has an append-tail overlay, point lookup still falls through
  to the existing overlay-aware exact path.
- Corrupt branch metadata, impossible page ids, or invalid internal fences
  return corruption rather than silently bypassing validation.
- Docs describe the exact-lookup fast path without claiming broader branch
  cursor streaming.

## Risks

- Computing the branch subtree maximum still traverses internal branch pages, so
  this slice primarily removes leaf-list allocation and unrelated first-leaf
  reads. A later branch cursor can avoid more repeated traversal for prefix and
  exact-entryset scans.
