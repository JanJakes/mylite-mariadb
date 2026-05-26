# Branch Snapshot Zeroed Leaf Encoding

## Problem Statement

Branch snapshot preparation now writes leaf pages directly into the final
branch-plus-leaf buffer, but the shared leaf encoder still clears each full
page before writing page metadata, cells, and checksum. The caller-owned
multi-page buffers used by `prepare_index_leaf_pages()` and
`prepare_index_branch_snapshot_pages()` are already allocated with `calloc()`,
so this extra page clear adds avoidable full-page memory traffic to the
prepared-insert branch-refold hot path.

This slice makes the multi-page leaf encoder consume zeroed page buffers
directly while preserving the clearing behavior for callers that encode into
stack or reused buffers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are involved.
- `prepare_index_leaf_pages()` allocates its page run with `calloc()`, then
  calls the multi-page leaf encoder.
- `prepare_index_branch_snapshot_pages()` allocates its branch-plus-leaf page
  run with `calloc()`, then calls the multi-page leaf encoder at the first leaf
  page offset.
- The multi-page leaf encoder currently calls `encode_index_leaf_page()` for
  each leaf, and `encode_index_leaf_page()` clears the full page before writing
  the leaf payload.
- Existing direct `encode_index_leaf_page()` callers still need a clearing
  wrapper because some pass stack buffers or buffers whose prior contents should
  not leak into unused page bytes.

## Design

- Split leaf encoding into a clearing wrapper and a zeroed-page encoder.
- Keep `encode_index_leaf_page()` as the safe wrapper for direct callers.
- Rename the multi-page helper to `encode_zeroed_index_leaf_pages()` and make
  it call the zeroed-page encoder because both current callers provide freshly
  `calloc()`-zeroed output buffers.
- Add a test hook counter for full-page clears performed by
  `encode_index_leaf_page()`.
- Extend branch snapshot and standalone leaf-page regressions to verify the
  multi-page encoder does not perform redundant page clears.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, file-format, or
wire-protocol behavior changes. Encoded pages remain byte-equivalent because
the skipped clear is replaced by the caller's zeroed allocation contract.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, locking, or companion-file changes.
This changes only transient page-encoding memory traffic before pages are
written into the primary `.mylite` file.

## Binary Size And Dependencies

No dependency changes. Code-size impact is limited to a small helper split and
internal test-hook coverage.

## Test And Verification Plan

- Add a test hook counter for clearing leaf-page encodes.
- Assert standalone leaf-page preparation and branch snapshot preparation do
  not call the clearing wrapper.
- Keep existing branch snapshot layout checks for page ids, fences, and leaf
  entry counts.
- Run the storage test target and CTest selection.
- Run formatting checks, storage-smoke embedded storage-engine coverage, and
  prepared-insert component performance baselines.

## Acceptance Criteria

- `encode_zeroed_index_leaf_pages()` writes into zeroed buffers without
  full-page clearing.
- Direct `encode_index_leaf_page()` callers keep their clearing behavior.
- Branch snapshot output remains layout-compatible with existing tests.
- Targeted tests and checks pass.

## Risks

- The main risk is accidentally using the zeroed-page encoder with a reused or
  stack buffer. Keeping the zeroed helper private to the multi-page encoder
  limits that contract to callers that already allocate fresh zeroed page runs.
