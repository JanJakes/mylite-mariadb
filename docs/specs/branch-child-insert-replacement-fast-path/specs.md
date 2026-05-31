# Branch Child Insert Replacement Fast Path

## Problem

Prepared-insert branch replacement fast paths handle the dominant level-1
branch rewrites where child count is unchanged: entry-count-only and
entry-count-plus-fence updates. The current smoke profile still reports `386`
structural `index-branch` dirty-buffer replacements, all at level `1`, after
leaf splits add one child to an already-resident branch page. Those replacements
fall back to a full `4096` byte page copy even though the encoded branch shape
is fixed-width and differs by one inserted child cell, one updated split-child
fence when the first split leaf's max key changes, and header count/checksum
fields.

The safe next slice is to prove that one-child branch insert replacement shape
inside the dirty-buffer replacement path and update the resident branch page in
place when the proof succeeds.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is modified.
- `split_branch_index_leaf_entry()` builds split leaf pages, derives child
  max fences with `copy_index_branch_children_with_split()`, encodes the
  replacement branch with `encode_index_branch_page_without_checksum()`, and
  writes it through the checksum-dirty maintained page path.
- `store_dirty_page_in_buffer()` currently tries leaf single-insert,
  branch same-child-count, and maintained-root single-insert replacement fast
  paths before copying the full page.
- Branch pages store fixed-width child cells at
  `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET`; child count and
  used-bytes fields are `u32`, entry count and checksum fields are `u64`.

## Design

Add a branch one-child insert dirty-buffer replacement proof:

- require both resident and incoming pages to be index branch pages with
  matching fixed metadata and key size;
- require child count and used bytes to grow by exactly one branch cell and
  entry count to grow by exactly one row entry;
- compare unchanged header bytes, level, padding, unused tail bytes, and
  unchanged child-cell prefix/suffix bytes;
- support both split shapes:
  - pure inserted child after an unchanged prefix; and
  - split-child fence update followed by the inserted child, where the original
    split child page id remains in place but its max row/key changes;
- update the resident payload with one `memmove()` plus one inserted-cell copy,
  copy the updated split-child fence when needed, and copy only the child-count,
  used-bytes, entry-count, and checksum fields.

If any proof check fails, keep the existing full-page replacement fallback. The
slice does not change branch split planning, journal validation, durable
checksum publication, or recovery behavior.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, file-format, or durable byte changes.

## Single-File And Lifecycle Impact

No file lifecycle, journal, recovery, lock, sidecar, or embedded lifecycle
change. The resident dirty-buffer branch page remains checksum-dirty until the
existing publication path refreshes it.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Production
code adds one narrow first-party proof helper. Test-hook builds add one scalar
counter and benchmark row for branch child-insert fast replacements.

## Tests And Verification Plan

- Add focused storage self-test coverage that stores a branch page in the dirty
  buffer, replaces it with a one-child split-shaped branch page, verifies the
  resident bytes match the incoming page, and asserts the branch child-insert
  fast replacement counter increments.
- Keep existing branch entry-count and entry-count-plus-fence fast path tests.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output reports branch child-insert fast
  replacements for the structural level-1 branch replacement path.
- Dirty-buffer replacement page-family totals, checksum totals, and protected
  maintained-root decode totals remain equivalent.
- Storage and embedded storage-engine smoke verification pass.

## Risks

- The proof assumes branch child cells remain fixed-width and contiguous from
  `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_PAYLOAD_OFFSET`. If branch cells become
  variable-width or gain interior padding, this helper must fall back.
- The helper must distinguish a real split-child fence update from unrelated
  structural branch rewrites; otherwise it could hide a malformed branch page
  that should use the full replacement path and later validation.

## Verification Result

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `310.23 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `329.64 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,982,738` bytes (`32.41 MiB`) with `478`
  members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step sampled at `73.603 us/op`. The profile reported `386`
  branch child-insert fast replacements, matching the `386` structural
  level-1 branch replacements from `split_branch_index_leaf_entry`. Structural
  counters stayed equivalent: `130,313` `index-branch` replacements, `8`
  full-page checksum calls, `227,063` zero-tail checksum calls, and `677`
  maintained-root decodes.
