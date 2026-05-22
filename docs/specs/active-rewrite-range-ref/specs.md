# Active Rewrite Range Ref

## Problem

Prepared point-update sampling over a small hot row set still shows
`rewrite_active_update_pages()` as the largest MyLite-owned storage frame. The
cached rewrite path has already proved that the replacement row, row-state, and
changed index pages are contiguous in the active append buffer, but the hot path
still checks that range and then resolves the row page and changed index page
through separate append-buffer lookups.

Those lookups repeat the same bounds and offset arithmetic inside every active
rewrite attempt.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The slice is first-party storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  needed.
- `rewrite_active_update_pages()` first calls
  `buffered_append_page_range_contains_in_statement()` for the replacement run,
  then calls `buffered_append_page_ref_in_statement()` for the row page, and on
  the cached single-index path calls it again for the index-entry page.
- The generic changed-index path repeats the same page-ref lookup for each
  changed index page after the range has already been accepted.
- Existing active rewrite tests cover repeated buffered updates, rollback, and
  checksum-refresh rollback behavior.

## Design

- Replace the boolean range-contained helper with an append-buffer range-ref
  helper that validates the same contiguous range once and returns pointers to
  the first page and first checksum-dirty slot.
- Use the returned range to construct the row-page ref and changed-index refs
  by offset in `rewrite_active_update_pages()`.
- Keep all existing cached-shape validation, row/state/index decode fallback,
  undo capture, page mutation, dirty-checksum marking, and no-rewrite fallback
  behavior.
- Keep other append-buffer page helpers unchanged for non-rewrite callers.

## Affected Subsystems

- MyLite storage active append-buffer rewrite path.
- Prepared update storage performance baseline.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, rollback, or
file-format behavior changes. The optimization reuses the same append-buffer
pages after proving the same range that the old code proved.

## Single-File And Embedded Lifecycle Impact

No durable state, companion file, lock, journal, recovery, or lifecycle change.
The range ref is transient process memory during one active statement rewrite.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party helper replacement. No dependency change.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000`
- Focused macOS sample of the same hot-row prepared-update phase.

Completed verification:

- `git diff --check`: pass.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: pass.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: pass.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  pass, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: pass, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000`: prepared primary-key
  update step measured 1.710 us/op.
- Focused macOS sample written to
  `/tmp/mylite-active-rewrite-range-ref.sample.txt`; the sample still shows
  row/index page rewrite and copy cost, but no named buffered range/page-ref
  helper frames on the hot path.

## Acceptance Criteria

- `rewrite_active_update_pages()` resolves a contiguous active append-buffer
  rewrite range once and derives row/index page refs from that range.
- Cached and uncached rewrite behavior remains covered by existing storage and
  embedded storage-engine tests.
- Rollback-sensitive active rewrite tests continue passing.
- Benchmark/profile evidence records the resulting hot-path shape.

## Risks And Unresolved Questions

- This only removes repeated append-buffer range/ref checks. The actual row and
  index payload copies, exact-index lookup, MariaDB prepare/open-table work, and
  rollback preimage capture remain separate costs.
