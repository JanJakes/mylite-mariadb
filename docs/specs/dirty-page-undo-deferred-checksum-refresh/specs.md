# Dirty Page Undo Deferred Checksum Refresh

## Problem

After routing maintained branch reads through cache-aware readers, the
prepared-insert benchmark still refreshes `3,318` dirty buffered pages under
the `dirty-page-copy` checksum source. The new undo write-site counters show
those dirty copies are preimages for dirty-page undo capture:

- `redistribute_branch_index_leaf_range_entry / index-leaf`: `3,000` dirty
  copies.
- `split_branch_index_leaf_entry / index-leaf`: `75` dirty copies.
- `split_branch_index_leaf_entry / index-branch`: `243` dirty copies.

Undo capture needs rollback bytes, not an immediately checksum-valid read
result, when the preimage comes from an active dirty-page buffer. The current
generic read path refreshes the dirty buffer checksum before copying it, so the
insert hot path pays rollback checksum work even when the statement commits.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite owns pager dirty-page rollback in
  `packages/mylite-storage/src/storage.c`.
- `capture_dirty_page_undo_for_pager_write()` protects a page in the recovery
  journal, reads the current page image through `read_page_at()`, and appends a
  full-page preimage through `append_dirty_page_undo()`.
- `read_page_at()` checks `copy_dirty_page_buffer()` before durable reads.
  `copy_dirty_page_buffer()` refreshes a checksum-dirty dirty-buffer page
  before copying it, and clears the dirty mark on the live dirty-buffer entry.
- `restore_dirty_page_undos()` writes stored undo pages back through
  `write_page_at_raw()` during statement rollback.
- `mylite_storage_buffered_page_undo` already carries a transient
  `checksum_dirty` flag for append-buffer rollback preimages; dirty-page undo
  entries do not.

## Design

- Add a transient `checksum_dirty` flag to `mylite_storage_dirty_page_undo`.
- Add a dirty-page-buffer undo copy helper that:
  - searches the active statement chain for the page;
  - records the existing dirty-page copy context counters;
  - copies the buffered bytes without refreshing the checksum;
  - returns the copied page's dirty-checksum state.
- Use that helper first in `capture_dirty_page_undo_for_pager_write()`. Fall
  back to the existing `read_page_at()` path only when no dirty-buffer preimage
  exists.
- Append dirty-page undo entries with the captured dirty-checksum state.
- During `restore_dirty_page_undos()`, refresh a dirty undo preimage into a
  local page copy before writing it back to the primary file.
- Add a test-hook checksum refresh source for deferred dirty-page undo restore
  so rollback refresh work remains observable without counting it as hot-path
  copy refresh work.

This slice only defers checksum repair for rollback preimages. Generic reads
of dirty buffered pages still refresh before returning checksum-valid bytes.

## Affected Subsystems

- MyLite pager dirty-page undo capture and rollback.
- Dirty-page-buffer checksum refresh accounting.
- Prepared-insert performance benchmark output.

No MariaDB SQL-layer, handler-layer, catalog, DDL metadata, or wire-protocol
code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or MySQL/MariaDB compatibility claim changes. Statement rollback must restore
the same logical page bytes with valid checksums as before.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. No journal format,
companion-file, lock, recovery ordering, or embedded lifecycle change. The new
dirty flag is transient process memory owned by active statement undo entries.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is a small first-party helper and one `int` on transient
dirty-page undo entries.

## Test And Verification Plan

- Extend dirty-page undo capture coverage to prove a dirty-buffer preimage is
  captured without a `dirty-page-copy` checksum refresh, keeps the live dirty
  page marked dirty, and restores a checksum-valid page on rollback.
- Run the prepared-insert benchmark and confirm the hot-path `dirty-page-copy`
  refresh count drops while dirty-page undo copy context counters remain
  attributable by write site.
- Keep storage and routed embedded storage-engine tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Results

Run on the VPS continuation environment with GCC 14.2:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `331.84 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size was `33,968,682` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `364.69 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step was `61.202 us/op`.

Benchmark evidence:

- `dirty-page-copy` checksum refreshes dropped from `3,318` to `0`.
- `dirty-page-undo-restore` checksum refreshes were `0` in the committing
  prepared-insert benchmark.
- Dirty-page undo-capture copy context attribution remained:
  - `index-leaf`: `3,075` copies, `3,075` dirty copies.
  - `index-branch`: `384` copies, `243` dirty copies.
- Undo write-site attribution remained:
  - `redistribute_branch_index_leaf_range_entry / index-leaf`: `3,000`
    copies, `3,000` dirty copies.
  - `split_branch_index_leaf_entry / index-leaf`: `75` copies, `75` dirty
    copies.
  - `split_branch_index_leaf_entry / index-branch`: `384` copies, `243` dirty
    copies.

## Acceptance Criteria

- Dirty-page undo capture can copy checksum-dirty dirty-buffer preimages
  without refreshing the live dirty buffer in the insert hot path.
- Rollback refreshes dirty undo preimages before writing them to the primary
  file.
- Generic dirty-buffer reads still return checksum-valid pages.
- Prepared-insert benchmark output shows the hot-path dirty-page-copy refresh
  count reduced while the undo write-site counters still explain captured
  preimage copies.
- Existing storage and routed embedded storage-engine tests pass.

## Risks

The optimization relies on the undo entry carrying the dirty-checksum state
until rollback. Missed propagation through nested savepoint merge would restore
a stale checksum, so merge coverage and direct rollback validation are required
before committing this slice.
