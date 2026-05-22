# Single-Index Rewrite Range Undo

## Problem

The prepared update benchmark commonly changes one fixed-width secondary key
and rewrites the current unpublished row plus changed index-entry page in the
active append buffer. The cached one-index path already avoids the generic
changed-index loop, but it still captures row and index rollback preimages as
whole meaningful prefixes before mutating either page.

For same-size row and same-size key rewrites, the page headers, row id, table
id, index number, payload length, and key length stay unchanged. Only the row
payload bytes and index key bytes need to be restored for statement rollback,
plus the checksum slots when dirty checksums are refreshed by reads before
rollback.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file changes are
  required.
- `rewrite_active_update_pages()` dispatches cached one-index rewrites to
  `rewrite_active_single_index_update_page()` after the buffered row, row-state,
  and changed index-page shape has already been validated and cached.
- `rewrite_active_single_index_update_page()` currently captures row and index
  preimages through `capture_buffered_page_undo_pair_from_pages()`, then calls
  `rewrite_buffered_row_page()` and `rewrite_buffered_index_entry_page()`.
- `capture_buffered_page_undo_range_from_page()` can now capture an offset
  range and upgrade a prior compact undo entry to a prefix undo when a later
  same-savepoint rewrite needs the conservative restore path.

## Design

- In the cached one-index rewrite helper, compare the old row size and old
  index key size with the new row and key sizes.
- If both sizes are unchanged:
  - capture the row range from `MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET`
    through the row payload end;
  - capture the index-entry range from
    `MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET` through the key end;
  - copy only the new row payload and key bytes;
  - preserve the existing dirty-checksum behavior by marking both pages dirty.
- If either size changes, keep the existing pair prefix-undo and full rewrite
  path.
- Rely on the existing range-to-prefix upgrade when a later rewrite in the same
  savepoint follows a same-size compact capture with a size-changing rewrite.

## Affected Subsystems

- MyLite cached one-index active append-buffer rewrite path.
- Statement/savepoint rollback for buffered row and index-entry pages.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, metadata, or durable
file-format behavior changes. The optimization is selected only after existing
cached-shape validation proves the row and index page layout.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only reduces transient in-memory rollback preimage copying for
active buffered row and index-entry pages.

## Public API And File-Format Impact

No public API, internal storage API, or durable file-format change.

## Binary-Size And Dependency Impact

Small first-party C branch in the cached one-index rewrite helper. No
dependency or build-profile change.

## Tests And Verification

- Add storage coverage for same-size cached one-index rewrite rollback after
  reads refresh dirty buffered row and index-entry checksums.
- Add same-savepoint coverage where a same-size one-index rewrite is followed
  by a row-size-changing one-index rewrite, proving compact row and index undo
  entries upgrade to the existing prefix restore path.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Rebuild the MariaDB storage-smoke archive when `storage.c` changes, then
  relink the embedded storage-engine and benchmark targets.
- Run storage unit, focused storage-engine CTest, full storage-smoke CTest,
  `git diff --check`, `git clang-format --diff`, and prepared-update
  performance baselines.

## Acceptance Criteria

- Cached one-index active rewrites with unchanged row and key sizes capture row
  and index checksum-plus-payload/key ranges instead of prefix preimages.
- Rollback restores row bytes and index visibility after reads refresh dirty
  checksums before rollback.
- A later size-changing rewrite in the same savepoint still rolls back to the
  original row and index bytes by upgrading compact undo entries.
- Size-changing row or key rewrites keep the existing conservative prefix path.
- Existing storage, embedded storage-engine, transaction, and rollback tests
  pass.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  build libmariadbd.a` passed with the pre-existing libtool no-symbol warnings.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed after the
  MariaDB archive rebuild.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` recorded prepared primary-key updates
  at `2.439` and `2.423 us/op` after noisy warm-up samples.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 10000 1000000` recorded prepared primary-key updates
  at `2.753 us/op`.
- The benchmark result stayed in the same envelope as the immediately preceding
  row-only range-undo slice, so no standalone latency claim is made for this
  narrower copy reduction.

## Risks And Unresolved Questions

- The optimization relies on cached one-index shape validation. Uncached,
  multi-index, or key-size-changing rewrites must stay on the conservative path.
- This reduces one hot preimage copy span but does not change MariaDB prepared
  execution, quick planning, or the future need for navigable maintained
  indexes.
