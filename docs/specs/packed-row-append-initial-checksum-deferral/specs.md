# Packed Row Append Initial Checksum Deferral

## Problem

Prepared indexed inserts now pack fixed-size row payloads into active append
buffer row pages. The first row on each packed row page is encoded with a
durable checksum immediately, but the page stays in the append buffer and later
slot appends mark the same page checksum-dirty before publication. The prepared
insert profile therefore spends hot-path row `checksum_page_zero_tail()` work on
page images that are not published until append-buffer flush refreshes them.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code and does not change MariaDB handler semantics.
- `packages/mylite-storage/src/storage.c::write_packed_inline_insert_pages()`
  reserves the first packed row page in the active append buffer and calls
  `encode_packed_row_page()` for slot `0`.
- `append_cached_packed_inline_row()` later appends slots to that cached page,
  captures undo, updates the row count and payload, and sets the buffered page
  checksum-dirty flag.
- `validate_cached_packed_inline_row_page()` validates the cached page shape
  used by append planning; it does not require a current checksum.
- `flush_statement_append_page_buffer()` refreshes all append-buffer pages whose
  checksum-dirty flag is set before writing the page range to the file.
- The current prepared-insert profile reports row zero-tail checksum calls in
  the insert loop from packed row page encoding, plus append-buffer row dirty
  refreshes at insert-loop and commit flushes.

## Design

- Split packed row page encoding into a shared body that can leave the checksum
  field zero.
- Keep the existing checksum-valid `encode_packed_row_page()` for direct test
  hooks and any caller that needs an immediately valid page.
- Add a checksum-deferred packed row encoder for slot-`0` packed pages created
  inside `write_packed_inline_insert_pages()`.
- Mark the reserved append-buffer checksum-dirty slot immediately after the
  checksum-deferred encode.
- Keep cached packed row validation structural; append-buffer flush remains the
  durable checksum publication point.

## Affected Subsystems

- Active packed inline insert writer.
- Append-page buffer checksum refresh.
- Prepared-insert checksum profiling counters.

## Compatibility Impact

No SQL-visible or handler-reference behavior changes are intended. Packed row
references, row payload bytes, table metadata, duplicate checks, and index
maintenance are unchanged.

## Single-File And Lifecycle Impact

No new durable pages or sidecars are introduced. The primary `.mylite` file
still receives checksum-valid row pages because append-buffer flush repairs all
checksum-dirty pages before write.

## Public API And File-Format Impact

No public API, on-disk layout, checksum algorithm, or page-format version
change. The deferred checksum state is transient in-memory append-buffer
metadata only.

## Storage-Engine Routing Impact

No routing policy change. Routed durable tables that already use active packed
inline inserts benefit from less hot-path checksum work.

## Binary-Size Impact

Small first-party storage helper split only. No dependency change.

## Tests And Verification Plan

- Add focused storage test-hook coverage that:
  - creates a slot-`0` packed row through `write_packed_inline_insert_pages()`;
  - proves the append-buffer page checksum slot is zero and marked dirty;
  - proves no row zero-tail checksum is charged to initial encode;
  - flushes the append buffer and validates the durable row page checksum.
- Run:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`

## Acceptance Criteria

- Slot-`0` packed row append-buffer pages are checksum-dirty until flush.
- Append-buffer flush publishes checksum-valid packed row pages.
- Prepared-insert benchmark output shows `encode_packed_row_page` no longer
  contributes row zero-tail checksum calls in the insert loop.
- Maintained-root planning and recovery-journal validation decode/checksum
  boundaries are unchanged.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `316.03 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `331.96 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, producing `libmariadbd.a` at `33,980,162` bytes (`32.41 MiB`) with
  `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The prepared insert step was `78.862 us/op`; full-page checksum
  calls stayed at `8`, zero-tail checksum calls fell to `227,438`, insert-loop
  row zero-tail calls fell to `4,441`, and `encode_packed_row_page` no longer
  appears in the checksum call-site table. Maintained-root decode sites stayed
  at `677` total: `674` under `plan_maintained_index_root_inserts`, `2` under
  `validate_recovery_journal_saved_page`, and `1` under
  `read_index_leaf_run_root`.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.

## Risks And Unresolved Questions

- If another caller starts using checksum-deferred packed row pages outside the
  append buffer, it must carry an equivalent checksum-dirty publication flag.
  This slice keeps the deferred helper local to the append-buffer writer.
- Packed index-entry first-page checksums remain a separate possible follow-up.
