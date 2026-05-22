# Single Index Rewrite Hot Inline

## Problem

Prepared primary-key update profiling is now dominated by
`rewrite_active_single_index_update_page()` and its rollback preimage capture.
That path is the expected hot path for repeated updates that change exactly one
secondary key and rewrite the current row/index pages in the active append
buffer.

The page preimage copies cannot be removed without redesigning statement
rollback. The narrower safe improvement is to remove helper call overhead around
the single-call rewrite path while preserving the exact same undo capture,
buffered page mutation, and dirty-checksum behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `rewrite_active_update_pages()` calls
  `rewrite_active_single_index_update_page()` only for a cached active rewrite
  shape with exactly one changed index.
- `rewrite_active_single_index_update_page()` calls
  `capture_buffered_page_undo_pair_from_pages()` before mutating either page.
- `capture_buffered_page_undo_pair_from_pages()` uses
  `init_buffered_page_undo_entry()` to copy row and index preimages into the
  statement rollback list. Those preimages are restored by
  `restore_buffered_page_undos()` on statement rollback.

## Design

- Mark `rewrite_active_single_index_update_page()` as
  `MYLITE_STORAGE_HOT_INLINE`.
- Mark its single-call pair undo helper as `MYLITE_STORAGE_HOT_INLINE`.
- Mark `init_buffered_page_undo_entry()` as `MYLITE_STORAGE_HOT_INLINE` so the
  hot pair capture path does not add a wrapper around the preimage copy.
- Preserve all existing duplicate undo checks, capacity checks, preimage copy
  sizes, checksum-dirty tracking, row rewrite, index rewrite, and result
  propagation.

## Affected Subsystems

- MyLite active append-buffer page rewrite.
- MyLite buffered-page statement rollback preimage capture.
- Prepared primary-key update active rewrite path.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, rollback, or
file-format behavior change. Statement rollback still restores the same buffered
page preimages.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only helper call shape in transient active append-buffer rewrites.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party inline annotation change on a hot single-call path. No
dependency or build-profile change.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check how the active rewrite
  and undo preimage copy frames move.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

Completed verification:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` recorded prepared primary-key updates
  at 2.263 us/op.
- The focused sampled run recorded 2.270 us/op in
  `/tmp/mylite-single-index-rewrite-hot-inline.sample`.
  `rewrite_active_single_index_update_page()`,
  `capture_buffered_page_undo_pair_from_pages()`, and
  `init_buffered_page_undo_entry()` were not visible sampled frames. The
  rollback preimage copy work moved under `rewrite_active_update_pages()`.
  Remaining visible storage work was active rewrite copying, live-row-id
  durable promotion, exact-index cache replacement, and handler-side key
  preparation.
- `git diff --check` passed, and `git clang-format --diff` for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- The single-index active rewrite path preserves the same row/index page
  mutation and rollback preimage capture behavior.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This does not reduce the actual rollback preimage copy cost. A compact
  statement-undo representation may be useful later, but that requires a
  separate design because it affects rollback restore semantics.
