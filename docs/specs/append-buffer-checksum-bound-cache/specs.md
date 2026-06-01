# Append Buffer Checksum Bound Cache

## Problem

The current prepared-insert benchmark still reports append-buffer checksum
refreshes under the generic dirty checksum helper:

- `6,643` row-page append-buffer flush refreshes;
- `206` index-entry append-buffer flush refreshes; and
- `6` index-entry append-buffer copy refreshes.

Those checksum refreshes are required before append-buffer pages are copied as
checksum-valid pages or published to the primary file. The redundant part is
the generic refresh helper's page-family classification and metadata parsing:
packed row and packed index-entry append paths already know the page family and
used-byte span when they encode or append to a buffered page.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is involved.
- `write_statement_append_page_buffer()` calls
  `refresh_statement_append_page_buffer_checksums()` before appending buffered
  pages to the primary file.
- `copy_buffered_append_page()` refreshes a checksum-dirty append-buffer page
  before returning a checksum-valid page copy to readers.
- Packed row append pages are created by `encode_dirty_packed_row_page()` and
  extended by `append_cached_packed_inline_row()`. Both paths know row size,
  row count, and used bytes.
- Packed index-entry append pages are created by
  `encode_dirty_packed_index_entry_page()` and extended by
  `append_packed_index_entry_to_page()`. Both paths know key size, entry count,
  and used bytes.
- Existing rollback and active-update rewrite paths can mark append-buffer
  pages checksum-dirty without proving packed append bounds. Those paths must
  clear cached bounds and keep the generic parser fallback.

## Design

Carry optional checksum-bound facts beside each append-buffer checksum-dirty
slot:

- page family, limited to packed row and packed index-entry pages;
- used-byte span for the checksum-zero-tail helper; and
- a validity bit cleared whenever an append-buffer page is reserved, replaced,
  restored from undo, or dirtied by a caller that does not supply fresh facts.

Add an append-buffer entry-aware refresh helper used by append-buffer flush and
append-buffer copy:

- when a checksum-dirty append-buffer page has valid cached row or index-entry
  bounds, refresh the checksum with the cached family and used-byte span;
- record the same dirty checksum refresh source/family counters as the generic
  helper;
- keep the generic `refresh_dirty_buffered_page_checksum()` fallback whenever
  cached facts are absent, invalid, unsupported, or out of page bounds; and
- keep durable readers, recovery-journal validation, dirty-page publication,
  maintained-root planning validation, and append-buffer ownership semantics
  unchanged.

The checksum bytes and publication timing do not change. This slice removes
redundant writer-side metadata parsing only when the append-buffer writer has
maintained equivalent state for the same in-memory page image.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata behavior, transaction semantics, recovery
semantics, or error-surface changes.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or embedded lifecycle change.
Append-buffer pages still publish checksum-valid bytes before primary-file
writes and still repair checksum-dirty pages before checksum-valid reads.

## Safety Boundary

This slice does not remove checksum refreshes, checksum validation, journal
validation, maintained-root planning validation, or durable reader validation.
The cached-bound path applies only to append-buffer writer-owned pages and
falls back to the existing generic parser whenever the writer did not carry
valid row or index-entry bounds forward.

## Test And Verification Plan

- Add focused storage self-test coverage proving:
  - packed row append-buffer flush uses cached bounds;
  - packed index-entry append-buffer flush uses cached bounds; and
  - an append-buffer page dirtied without facts still uses the generic fallback.
- Print an append-buffer checksum-bound cache counter in the prepared-insert
  benchmark.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Append-buffer slots carry row/index-entry checksum bounds only when the writer
  has fresh facts for the current page image.
- Append-buffer flush and copy reuse cached bounds for valid packed row and
  packed index-entry pages while preserving checksum-source/family accounting.
- Fallback append-buffer pages still refresh through the generic parser.
- Prepared-insert structural counters stay unchanged, including checksum
  refresh counts and protected maintained-root decodes.

## Risks

- Stale append-buffer bounds would publish a checksum for the wrong page span.
  The implementation must clear facts through generic dirty-mark paths and set
  facts only in packed append writers that have just updated the page image.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `398.92 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `398.83 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,997,826` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed under unrelated high host load (`16.23`, `21.18`, `21.13`). The
  sampled prepared-insert step was `85.096 us/op`.

The benchmark reported `6,855` cached append-buffer refreshes (`6,849`
append-buffer flushes plus `6` append-buffer copies) and kept the structural
counters unchanged: `8` full-page checksum calls, `127,063` zero-tail checksum
calls, `5` protected maintained-root decodes, `21,031` dirty leaf pressure
admissions, `66,144` merge direct writes, `87,176` index-leaf dirty refreshes,
`31,938` pressure-context builds, and `19,053` planned stores.
