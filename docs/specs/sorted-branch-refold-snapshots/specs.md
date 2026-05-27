# Sorted Branch Refold Snapshots

## Problem

Branch-root refolds rebuild a branch snapshot from a refold entryset prepared
during insert planning. Cache-backed refold plans are already maintained in
`(key,row_id)` order, and non-cache plans can be normalized once during
planning. `prepare_index_branch_snapshot_pages()` still calls
`build_raw_index_entry_order_if_needed()` and scans every adjacent entry before
encoding the snapshot leaves. Local sampling after batched branch snapshot leaf
writes showed `build_raw_index_entry_order_if_needed()` and
`prepare_index_branch_snapshot_pages()` back on the prepared insert hot path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`.
- `build_branch_index_refold_insert_entryset_if_fit()` copies the active
  branch-refold cache or reads the published leaf entries, then inserts the new
  `(key,row_id)` with `insert_raw_index_entry_in_order_to_entryset()`. Cache
  copies are maintained in sorted order, but branch-root reads that include a
  live append tail can arrive as sorted static entries followed by unsorted tail
  entries.
- `update_branch_refold_entryset_cache_after_simple_branch_insert()` uses the
  same ordered insertion helper when keeping the cache alive across simple
  branch inserts.
- `mylite_storage_test_branch_refold_entryset_cache_roundtrip()` already
  verifies that cached and planned refold entrysets do not require an allocated
  raw-entry order.
- Generic snapshot callers such as index rebuilds and maintained-root overflow
  promotion can still receive raw append-history entrysets that are not known
  to be ordered.

## Design

Split branch snapshot preparation into a generic path and a sorted-entry path:

- keep `prepare_index_branch_snapshot_pages()` on the defensive
  `build_raw_index_entry_order_if_needed()` path;
- add a sorted wrapper used only when the caller has a proven ordered entryset;
- let the leaf encoder skip both the allocation and adjacent-entry probe when
  sorted order is explicit; and
- normalize non-cache refold plans in place before storing them on the insert
  plan, then route `refold_branch_index_root_insert()` through the sorted
  wrapper only when it uses the planned `insert->refold_entryset` for the same
  row id.

The fallback refold path that rereads the branch root stays on the generic
path unless a later slice proves all tail-overlay shapes sorted at that point.

## Non-Goals

- No page-format or checksum change.
- No change to branch refold selection.
- No change to generic rebuild ordering.
- No attempt to avoid leaf checksum generation.

## Compatibility Impact

No SQL, C API, handler, storage routing, metadata, or wire-protocol behavior
changes. Encoded branch and leaf pages remain byte-equivalent for sorted
entrysets.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The ordering proof is
process-local planner state and does not change file lifecycle, rollback, or
recovery semantics.

## Build, Size, And Dependencies

Small first-party C change only. No new dependency or embedded build-profile
change.

## Test Plan

- Extend storage test-hook coverage so sorted refold entrysets can encode a
  branch snapshot without probing raw-entry order.
- Cover normalization of an unsorted raw refold entryset before it uses the
  sorted snapshot encoder.
- Keep existing unsorted snapshot tests on the generic ordering path.
- Run the storage unit suite.
- Run storage-smoke embedded storage-engine coverage.
- Run the prepared insert component benchmark.
- Run a longer prepared insert component stress after implementation to catch
  branch refold publication corruption.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Planned refold entrysets skip `build_raw_index_entry_order_if_needed()`
  entirely during branch snapshot encoding.
- Generic snapshot preparation still sorts unsorted raw entrysets.
- Existing storage and routed storage-engine tests pass.
- The focused prepared insert component benchmark does not regress locally.

## Risks And Open Questions

- This removes one ordered-entry scan from refold snapshot encoding, but leaf
  checksums and full snapshot construction remain on the hot path.
- The fallback refold path may also be safely sorted, but it needs separate
  proof for branch roots with append-tail overlays before it should use the
  sorted wrapper.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed: 1/1 test, 161.70 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed: 1/1 test, 41.17 seconds.
- Four sequential runs of
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  measured `prepared insert step component` at `23.174`, `30.267`, `24.181`,
  and `32.612 us/op`.
- A stress run of
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=100000 1000`
  completed without the earlier branch-refold corruption and measured
  `prepared insert step component` at `65.607 us/op` with a 1.3 GB temporary
  database file and a large final commit. The stress run is treated as
  correctness evidence, not a steady-state local timing baseline.
