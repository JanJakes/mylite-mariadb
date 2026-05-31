# Prevalidated Branch Leaf Writes

## Problem

The current prepared-insert smoke profile still spends most hot storage work in
branch-maintained leaf publication. Branch leaf-range redistribution writes
`24,796` freshly encoded replacement leaves, and branch split paths write
newly encoded split leaves, through the generic pager page writer. That writer
must support any page family, so after the write it probes both active
index-leaf and active index-branch cache publication paths.

Those branch-maintenance callers already know the page image is an encoded
index leaf produced by the leaf encoder. MyLite already has
`pager_write_prevalidated_index_leaf_page()`, which preserves dirty-page undo
capture, durable writes, active leaf-cache publication, and stale dirty-buffer
discard while skipping the generic branch-page publication probe.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source changes are needed.
- `pager_write_page()` captures dirty-page undo, writes the page, publishes
  active leaf and branch cache entries when the page shape matches, and
  discards stale dirty-buffer entries.
- `pager_write_prevalidated_index_leaf_page()` captures the same undo, writes
  the page, publishes only the active leaf cache from the encoded page, and
  discards stale dirty-buffer entries.
- `redistribute_branch_index_leaf_range_entry()` writes pages returned by
  `prepare_zeroed_index_leaf_range_pages()`, which encodes table index leaves
  with valid checksums.
- The branch split writers write `split_second_leaf_page`, which is produced by
  `prepare_index_leaf_split_pages()` before branch/root publication.

## Design

Replace generic pager writes with `pager_write_prevalidated_index_leaf_page()`
only where the local caller owns a freshly encoded index-leaf page:

- leaf-range redistribution replacement leaves;
- appended second leaves in single-level branch split writers; and
- appended second leaves in the level-two/deeper branch split variants.

Do not change branch/root writes, dirty-buffer publication, checksum timing,
planning validation, recovery-journal validation, or durable read validation.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, transaction semantics, or error-surface changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The same page bytes are written with the same dirty-page undo
protection and dirty-buffer discard semantics.

## Tests And Verification Plan

- Reuse the existing storage self-tests covering branch leaf redistribution,
  branch leaf splits, rollback, recovery, and active index page caches.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Storage and storage-smoke tests pass.
- Prepared-insert structural counters stay equivalent: `24,796`
  leaf-range-encoded pages, `772` split-encoded leaf pages, `8` full-page
  checksum calls, `227,063` zero-tail checksum calls, and `677`
  maintained-root decodes remain the comparison baseline.
- The sampled prepared-insert timing is recorded, but the stable acceptance
  evidence is unchanged structural counters because this slice removes generic
  page-family probing rather than changing checksum or decode counts.

## Risks

- A caller must not use the prevalidated leaf writer for branch, root, row, or
  catalog pages. This slice only changes call sites whose input pages are
  produced by index-leaf encoders in the same function.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `318.89 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,984,626` bytes and `478` archive members.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `326.77 sec`.

The final storage-smoke prepared-insert profile
(`/tmp/mylite-prevalidated-branch-leaf-writes-benchmark-3.txt`) reported:

- prepared insert step: `83.748 us/op` on a timing-noisy host;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- dirty `index-leaf` refreshes: `87,176`;
- maintained-root decodes: `677`;
- index-leaf encode call sites unchanged at `24,796`
  `prepare_zeroed_index_leaf_range_pages`, `772`
  `prepare_index_leaf_split_pages`, and `4`
  `prepare_index_branch_snapshot_pages_with_order`; and
- branch writer decode counters unchanged at `0`.
