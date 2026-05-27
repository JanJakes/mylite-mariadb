# Direct Branch Snapshot Fences

## Problem

Branch snapshot preparation encodes a replacement leaf run and then allocates
three temporary child arrays so `encode_index_branch_page()` can build the
branch root: child page ids, child max row ids, and child max keys. For branch
snapshots, child page ids are contiguous and child max fences are already
present in the freshly encoded leaf pages. After sorted refold snapshot work,
sampling still shows `prepare_index_branch_snapshot_pages_with_order()` hot
under `refold_branch_index_root_insert()`, so this scratch-array path is
avoidable overhead in the remaining refold snapshot construction cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`.
- `prepare_index_branch_snapshot_pages_with_order()` returns a branch page
  followed by a contiguous leaf page run.
- `encode_zeroed_index_leaf_pages()` already writes each leaf's max entry in
  leaf order and can optionally return max fences through scratch arrays.
- `read_encoded_index_leaf_max_cell()` can read the max `(key,row_id)` fence
  directly from a trusted encoded leaf page without decoding or checksumming
  the page again.
- Branch snapshot leaf page ids are always `first_leaf_page_id + offset`.

## Design

Add a branch-page encoder for branch snapshots that derives each child cell
directly from the encoded leaf run:

- use the sequential leaf page id instead of a child-page-id scratch array;
- read the max `(key,row_id)` from each encoded leaf page instead of storing
  max-row and max-key scratch arrays;
- keep the existing generic `encode_index_branch_page()` API for callers that
  naturally have child arrays; and
- keep generic leaf encoding unchanged for callers that need returned max
  fences.

The new helper is internal and only used by
`prepare_index_branch_snapshot_pages_with_order()`.

## Non-Goals

- No page-format or checksum change.
- No change to leaf checksum generation.
- No change to generic branch-page encoding.
- No change to branch refold selection or B-tree maintenance strategy.

## Compatibility Impact

No SQL, C API, handler, storage routing, metadata, or wire-protocol behavior
changes. Encoded branch pages should be byte-equivalent to the scratch-array
path for the same leaf run.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. This only removes
statement-local scratch allocations while preparing pages that are already
durable-publication candidates.

## Build, Size, And Dependencies

Small first-party C change only. No new dependency or embedded build-profile
change.

## Test Plan

- Reuse existing branch snapshot layout coverage, which verifies branch child
  page ids and max fences from the encoded snapshot.
- Run storage unit coverage.
- Run storage-smoke embedded storage-engine coverage.
- Run the prepared insert component benchmark.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Branch snapshot preparation no longer allocates child page id, max row id, or
  max key scratch arrays.
- Branch snapshot branch pages preserve the same child ids and max fences.
- Existing storage and routed storage-engine tests pass.
- The focused prepared insert component benchmark does not regress locally.

## Risks And Open Questions

- This removes allocation and copy overhead but does not address the dominant
  per-leaf checksum work.
- Future multi-level snapshot preparation may need a separate direct-fence
  helper for lower branch pages.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed: 1/1 test, 144.40 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed: 1/1 test, 33.67 seconds.
- Four sequential runs of
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  measured `prepared insert step component` at `34.165`, `33.668`, `22.243`,
  and `22.049 us/op`. The first pair was noisy; the second pair returned to
  the prior local band. The benchmark emitted the known CSV-engine fallback
  message.
