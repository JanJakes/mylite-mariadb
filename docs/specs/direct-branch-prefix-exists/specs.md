# Direct Branch Prefix Exists

## Problem

Static prefix-existence checks are allocation-free after a leaf run is loaded,
but branch roots still enter through `read_index_leaf_run_root()`. That builds
the transient branch leaf run and reads the first child leaf before locating
the prefix range.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite storage uses `mylite_storage_index_prefix_exists_for_index()` for
  duplicate/FK-style prefix checks that only need to know whether a matching
  live row id exists outside an optional skipped row id.
- `packages/mylite-storage/src/storage.c::find_static_index_leaf_prefix_exists()`
  currently calls `read_index_leaf_run_root()` and then
  `scan_static_index_leaf_prefix_exists()`.
- Branch roots already expose prefix lower-bound child selection through
  `find_index_branch_prefix_child_page()`, and page-local prefix scans can stop
  once key order moves past the requested prefix.

## Design

- Add a static no-tail branch prefix-exists path in
  `find_static_index_leaf_prefix_exists()`.
- Reuse branch subtree maximum-page detection. If pages after the static branch
  subtree exist, leave the existing overlay-aware prefix-exists path in charge.
- Recursively choose the first branch child whose high key can contain the
  prefix, then scan that child and later siblings until a match is found or the
  prefix range ends.
- Keep the existing append-tail overlay path unchanged.

## Compatibility Impact

No SQL-visible behavior change. Prefix existence remains an internal storage
optimization and returns the same boolean result as the existing leaf-run path.

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

- Extend branch prefix coverage so prefix-existence checks succeed for a later
  branch prefix while an unrelated first child leaf is checksum-corrupted.
- Keep existing exact, prefix-entryset, full, and append-tail branch coverage.
- Run storage tests, storage smoke, whitespace checks, and clang-format diff
  checks for touched C files.

## Acceptance Criteria

- Static no-tail branch prefix-existence checks read only the selected prefix
  branch range.
- `skip_row_id` is honored across matching leaves.
- Append-tail overlays still use the existing overlay-aware path.
- Corrupt branch metadata, invalid internal fences, invalid leaf metadata, or
  out-of-order leaf keys return corruption.

## Risks

- Prefix ranges can span many leaves. A later shared branch cursor abstraction
  can reduce duplication between exact, prefix, full, and prefix-exists readers.
