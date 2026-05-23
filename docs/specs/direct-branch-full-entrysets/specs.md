# Direct Branch Full Entrysets

## Problem

`mylite_storage_read_index_entries()` still reads branch roots through the
generic leaf-run reader. For branch roots, that reader first collects every
branch child leaf page id into a transient list before reading the leaves. Full
index reads must still read every leaf, but they do not need a separate full
leaf-id materialization step for static no-tail branch roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` builds full index cursors from
  `mylite_storage_read_index_entries()` for durable MyLite indexes.
- `packages/mylite-storage/src/storage.c::read_index_leaf_entries()` currently
  calls `read_index_leaf_run_root()` for branch roots, which invokes
  `collect_index_branch_leaf_run_pages()` before appending entries.
- `packages/mylite-storage/src/storage.c::append_index_leaf_entries_to_entryset()`
  already provides the page-local append operation needed by a streaming branch
  full reader.

## Design

- Add a branch-root full-entryset path in `read_index_leaf_entries()`.
- Reuse branch subtree maximum-page detection. If pages after the static branch
  subtree exist, fall through to the existing leaf-run reader so append-tail
  overlays keep the current overlay-aware full-index path.
- For no-tail branch roots, recursively follow branch children in stored order,
  read each leaf once, validate metadata and page ordering, and append entries.
- Preserve existing corruption checks for branch child metadata, branch fences,
  leaf metadata, leaf ordering, and branch-owned entry counts.
- Keep exact, prefix, and prefix-existence behavior unchanged.

## Compatibility Impact

SQL-visible behavior does not change. Full index reads keep returning entries
in branch key/row-id order, matching the existing leaf-run path.

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

- Use storage branch coverage where full entryset reads exercise branch roots
  with stored child page ids and validate complete ordered output.
- Keep existing exact, prefix, prefix-existence, and append-tail branch
  coverage.
- Run storage tests, storage smoke, whitespace checks, and clang-format diff
  checks for touched C files.

## Acceptance Criteria

- Static no-tail branch full entryset reads stream branch leaves directly
  without materializing the full transient branch leaf list.
- Returned entries remain complete and ordered.
- Append-tail overlays still fall through to the existing leaf-run
  overlay-aware path.
- Corrupt branch metadata, invalid internal fences, invalid leaf metadata, or
  out-of-order leaves return corruption.

## Risks

- This removes one allocation step but still reads every leaf. Larger full-scan
  wins will come from cursor reuse, caching, or planner-level avoidance of full
  scans.
