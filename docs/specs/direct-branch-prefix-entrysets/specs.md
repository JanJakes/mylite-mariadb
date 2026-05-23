# Direct Branch Prefix Entrysets

## Problem

`mylite_storage_read_index_prefix_entries()` still reads branch roots through
the generic leaf-run path. That path builds the full transient branch leaf list
before prefix filtering, even when branch fences can identify the first child
that can contain the prefix and scanning can stop as soon as keys move past the
prefix.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` exposes prefix index access through
  `index_read_map()` read modes such as `HA_READ_PREFIX`, with cursor
  continuation through handler APIs rather than an engine-mandated page shape.
- `mariadb/storage/mylite/ha_mylite.cc` builds filtered prefix cursors from
  `mylite_storage_read_index_prefix_entries()` for durable MyLite indexes.
- `packages/mylite-storage/src/storage.c::read_index_leaf_prefix_entries()`
  currently calls `read_index_leaf_run_root()` for branch roots, so branch full
  leaf-list materialization happens before prefix filtering.
- `packages/mylite-storage/src/storage.c::find_index_branch_prefix_child_page()`
  and `append_index_leaf_prefix_matches_to_entryset()` already provide the
  branch lower-bound and page-local prefix filtering needed for a bounded
  branch prefix reader.

## Design

- Add a branch-root prefix-entryset path in
  `read_index_leaf_prefix_entries()`.
- Reuse branch subtree maximum-page detection. If pages after the static branch
  subtree exist, fall through to the existing leaf-run reader so append-tail
  overlays keep the current overlay-aware prefix path.
- For no-tail branch roots, recursively choose the first child whose high key
  can contain the requested prefix, then scan that child and later siblings in
  branch order until page-local prefix filtering reports that the scan is past
  the prefix.
- Preserve branch child validation for lower branch pages.
- Keep exact, full-index, and prefix-existence behavior unchanged.

## Compatibility Impact

SQL-visible behavior does not change. Prefix index reads keep returning matching
entries in branch key/row-id order, matching the existing leaf-run path.

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

- Add storage coverage for a branch snapshot where a requested prefix spans
  multiple later leaves while an unrelated earlier leaf is checksum-corrupted.
  Prefix entryset reads must succeed without reading the earlier leaf.
- Keep existing full, exact, prefix, and prefix-existence branch coverage.
- Run storage tests, storage smoke, whitespace checks, and clang-format diff
  checks for touched C files.

## Acceptance Criteria

- Static no-tail branch prefix entryset reads stream only the selected prefix
  range instead of materializing the full branch leaf list.
- Prefix matches spanning adjacent branch leaves are returned completely and in
  order.
- Append-tail overlays still fall through to the existing leaf-run
  overlay-aware path.
- Corrupt branch metadata or invalid internal fences return corruption.

## Risks

- Prefix ranges can legitimately span many leaves. A later shared branch cursor
  abstraction can reduce duplication between exact, prefix, and full scans.
