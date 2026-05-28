# Dirty Page Buffer Buckets

## Problem

Prepared insert storage now attributes the remaining maintained-index dirty
buffer churn to repeated rewrites of pages that are already resident in the
dirty-page buffer. The current dirty-page buffer preserves the right nested
statement and rollback behavior, but page lookup is a linear scan. In the
current prepared-insert component smoke profile, `insert_branch_index_leaf_entry`
accounts for `64,881` dirty `index-leaf` replacements and `122,238` dirty
`index-branch` replacements. Those writes repeatedly search the protected-page
window before replacing an existing buffered page or merging a nested statement
buffer into its parent.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- `mylite_storage_dirty_page_buffer` stores protected maintained-index page
  rewrites for active statements. `store_dirty_page_in_buffer()` currently
  calls `dirty_page_buffer_entry()` to find an existing page before appending or
  replacing a pressure-flushed slot.
- Nested statement commit calls `merge_dirty_page_buffer()`, which copies each
  child dirty page into the parent through the same store helper. That preserves
  statement rollback isolation and must not be bypassed by writing directly into
  a parent buffer.
- The active leaf/branch page caches are stored on the root active statement,
  while dirty-page buffers belong to the current checkpoint and merge upward on
  successful nested commit. Direct in-place mutation of the parent dirty buffer
  would require new rollback preimage handling and is out of scope for this
  slice.

## Design

Add an optional page-id bucket table to `mylite_storage_dirty_page_buffer`,
using the existing storage hash/bucket conventions:

- keep linear lookup for small buffers;
- build buckets once the buffer reaches the small-cache threshold;
- use bucket lookup for `dirty_page_buffer_entry()` and
  `const_dirty_page_buffer_entry()` when buckets are present;
- maintain buckets incrementally on append, pressure-slot replacement, and
  swap-removal discard;
- reset bucket heads when a flush empties the buffer, while retaining allocated
  arrays for reuse;
- free bucket arrays when the dirty buffer is cleared.

The buffer still stores page bytes inline in each entry. This slice deliberately
does not change nested statement ownership, rollback preimages, pressure
eviction policy, checksum-dirty state, or page publication order.

## Compatibility Impact

No SQL, C API, handler API, storage-engine routing, DDL metadata, or file-format
behavior changes. This is an internal lookup optimization for active
maintained-index dirty-page buffers. `ENGINE=InnoDB` continues to route through
MyLite storage under the existing storage-smoke coverage.

## Single-File And Lifecycle Impact

No new durable or transient files are introduced. Dirty-page buffers remain
process-memory state owned by active storage checkpoints and are flushed to the
primary `.mylite` file through the existing statement commit and buffer-pressure
paths.

## Binary Size And Dependency Impact

No new dependencies. The binary-size impact is limited to a few helper
functions and transient pointer arrays inside the existing dirty-buffer
allocation.

## Tests And Verification

- Add a storage test-hook case that fills the dirty-page buffer enough to build
  buckets, verifies lookup after replacement, verifies lookup invalidation after
  swap-removal discard, verifies pressure-slot replacement relinks the new page
  id, and verifies flush empties lookup state.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Dirty-page buffer lookup remains correct across append, same-page
  replacement, pressure-slot reuse, discard, flush, and cleanup.
- Nested dirty-buffer merge and rollback semantics are unchanged.
- Existing storage and embedded storage-engine smoke tests pass.
- Prepared-insert component output remains behaviorally consistent, with the
  replacement churn still visible through existing counters.

## Risks

- Bucket maintenance bugs could strand stale page ids after swap-removal or
  pressure replacement. The focused test covers those mutations directly.
- The slice reduces lookup complexity but does not remove full-page copies
  between nested statement buffers. A future larger slice would need explicit
  rollback design before changing dirty-page ownership or in-place mutation.
