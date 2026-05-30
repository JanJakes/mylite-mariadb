# Branch Leaf Split Branch Checksum Deferral

## Problem

The current prepared-insert benchmark reports only `390` `index-branch`
zero-tail checksum calls. The remaining writer-side hot site is:

- `386` calls under `encode_index_branch_page`.

Source inspection shows those calls come from the single-level branch leaf-split
writer. That path already validates the durable branch root and selected leaf
before mutation, carries the split leaf max fences from leaf encoding, validates
the branch encoder inputs with `validate_index_branch_encode_components()`, and
then writes the existing branch root page. The checksum is redundant while the
page remains statement-owned and can be refreshed by the maintained dirty-page
publication path.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes first-party
  MyLite storage code only.
- `split_branch_index_leaf_entry()` reads the existing branch root and selected
  leaf through the branch insert writer readers. Cache misses still run durable
  checksum validation.
- The function splits the leaf with `prepare_index_leaf_split_pages()`, carries
  both split leaf max fences into `copy_index_branch_children_with_split()`,
  validates the new branch child arrays with
  `validate_index_branch_encode_components()`, and encodes the replacement
  branch page.
- The replacement branch page rewrites an existing maintained index branch root.
  `pager_write_maintained_insert_page()` can stage existing maintained index
  pages in the dirty-page buffer with `checksum_dirty=1`, and direct fallback or
  dirty-buffer flush refreshes the checksum before durable publication.
- Planning, recovery-journal saved-page validation, and future durable branch
  reads stay on checksum-validating decode paths.

## Design

Split branch-page assembly from checksum publication:

- add an internal branch-page encoder helper that writes the exact branch page
  bytes but leaves the checksum slot zero;
- keep the existing `encode_index_branch_page()` API as the checksum-valid
  wrapper for all existing callers; and
- use the checksum-free helper only in `split_branch_index_leaf_entry()` after
  targeted component validation, then write the existing branch root through
  `pager_write_maintained_insert_page(..., checksum_dirty=1)`.

The split leaf pages still use their existing checksum-valid publication path.
New branch pages, branch-root promotion, branch snapshot publication, planning,
and journal validation are unchanged in this slice.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
metadata layout, file format, or checksum algorithm changes. Durable branch
pages are still published with valid checksums.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The zero-checksum branch
page exists only in statement-local dirty-buffer or cache state, using the same
rollback, savepoint, flush, close, and recovery lifecycle as earlier maintained
branch checksum deferral work.

## Binary-Size And Dependency Impact

No dependency or license changes. The binary impact is limited to one internal
encoder helper and focused storage self-test coverage.

## Test And Verification Plan

- Extend the branch leaf-split validation self-test to prove the checksum-free
  branch encoder records no zero-tail checksum calls, leaves a zero checksum
  slot, and becomes checksum-valid after the existing dirty checksum refresh
  helper runs.
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

- The existing `encode_index_branch_page()` API still emits checksum-valid
  branch pages for ordinary callers.
- `split_branch_index_leaf_entry()` no longer computes an immediate branch
  checksum for the rewritten existing branch root.
- Prepared-insert checksum call-site output removes the prior `386`
  `encode_index_branch_page / index-branch` zero-tail calls without reducing
  planning or journal validation coverage.
- Storage and storage-smoke verification pass.

## Implementation Evidence

Implemented with `encode_index_branch_page_without_checksum()` behind the
existing checksum-valid `encode_index_branch_page()` wrapper. The only writer
caller is `split_branch_index_leaf_entry()`, after targeted branch component
validation and before the page is staged through
`pager_write_maintained_insert_page(..., checksum_dirty=1)`. The storage
self-test now checks that the checksum-free encoder records no zero-tail
checksum calls, leaves the branch checksum slot zero, and becomes
checksum-valid after `refresh_dirty_buffered_page_checksum()`.

The prepared-insert benchmark after this slice reports:

- prepared insert step: `81.586 us/op` with unrelated high-CPU jobs active, so
  timing is structural-only evidence;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `234,864`;
- `index-branch` zero-tail checksum calls: `4`;
- `index-branch` dirty checksum refreshes: `2`;
- maintained-root decodes: `677`, unchanged across protected planning,
  journal-validation, and root-to-leaf conversion sites;
- `encode_index_branch_page` no longer appears in the branch zero-tail
  checksum call-site table.

Verification passed:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The static storage-smoke archive measured `33,979,226` bytes (`32.41 MiB`).

## Risks

A rewritten branch page can carry a zero checksum while resident in the active
statement dirty buffer or active branch cache. That is acceptable only for this
writer-local path because the branch and leaf source pages were already
validated, the replacement child arrays are targeted-validated, and the
existing dirty checksum publication path refreshes the checksum before durable
write or generic read exposure.
