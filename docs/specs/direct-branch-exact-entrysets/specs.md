# Direct Branch Exact Entrysets

## Problem

Static no-tail branch point lookup now reads only the selected target leaf, but
`mylite_storage_read_exact_index_entries()` still enters branch roots through
the generic leaf-run reader. That reader builds the full transient branch leaf
list before exact filtering, even when the requested key can only occupy a
small consecutive range of leaves.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` exposes non-unique exact-key cursor behavior through
  `index_read_map()` followed by `index_next_same()`. The handler API expects
  ordered matching rows, not a particular on-disk cursor shape.
- `mariadb/storage/mylite/ha_mylite.cc` builds exact-key storage cursors from
  `mylite_storage_read_exact_index_entries()` before materializing rows.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_entries()`
  currently uses `read_index_leaf_run_root()` for branch roots. That path
  recursively collects every leaf page id before
  `append_index_leaf_run_matches_to_entryset()` scans matching leaves.
- `packages/mylite-storage/src/storage.c::find_index_branch_child_page()` and
  `find_index_branch_leaf_page()` already use branch high `(key, row_id)`
  fences to find the first leaf whose maximum can contain an exact key.

## Design

- Add a branch-root exact-entryset path in `read_index_leaf_exact_entries()`.
- Reuse branch subtree maximum-page detection. If pages after the static branch
  subtree exist, fall through to the existing leaf-run reader so append-tail
  overlays keep the current overlay-aware exact path.
- For no-tail branch roots, recursively choose the first child whose high fence
  can contain `(key, row_id=0)`, then scan that child and later sibling
  branches/leaves in branch order.
- Read leaf pages one at a time, append exact matches, and stop once a leaf's
  first key is greater than the requested key or its last key no longer equals
  the requested key after matches.
- Preserve internal branch-child validation: lower branch pages must match
  table/index/key metadata, be exactly one level below the parent, and have a
  high fence matching the parent cell.

## Non-Goals

- No prefix entryset streaming. Prefix reads still use the generic leaf-run
  path until a dedicated prefix cursor slice lands.
- No full-index streaming rewrite.
- No branch mutation, sibling-pointer, compaction, or file-format change.

## Compatibility Impact

SQL-visible behavior does not change. Non-unique exact index reads keep
returning matching row ids in branch order, which is the same key/row-id order
provided by the existing leaf-run path.

## Single-File And Lifecycle Impact

No file-format change and no new sidecar. The slice only changes how existing
branch and leaf pages are read from the primary `.mylite` file.

## Public API, Storage Routing, And Wire Protocol

No public `libmylite` API, storage-engine routing, SQL policy, or wire-protocol
change.

## Binary Size And Dependencies

Small first-party recursive readers only. No dependency and no meaningful
binary-size impact.

## Tests And Verification

- Add storage coverage for a branch snapshot where the requested duplicate key
  spans more than one leaf, while an unrelated earlier leaf is checksum
  corrupted. Exact entryset reads for the later key must succeed, proving the
  branch path no longer reads unrelated earlier leaves.
- Keep full and prefix branch coverage on existing tests.
- Run storage tests, storage smoke, whitespace checks, and clang-format diff
  checks for touched C files.

## Acceptance Criteria

- Static no-tail branch exact entryset reads append matches by reading only the
  selected key range leaves.
- Duplicate keys spanning adjacent branch leaves are returned completely and in
  order.
- Append-tail overlays still fall through to the existing leaf-run
  overlay-aware path.
- Corrupt branch metadata or invalid internal fences return corruption.

## Risks

- The recursive scan can still walk sibling branch pages while a key spans many
  leaves. A later full cursor abstraction can make exact, prefix, and full scans
  share one incremental branch iterator.
