# Packed Index Entry Append Initial Checksum Deferral

## Problem

The prepared-insert checksum profile still reports
`encode_packed_index_entry_page` zero-tail checksum calls for first entries in
packed append-buffer index-entry pages. Those pages are transient append-buffer
pages, and later packed index-entry appends already mark them checksum-dirty
before publication. The initial checksum is therefore redundant when the page is
kept in the append buffer and flushed through the normal dirty checksum path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This is first-party MyLite
  storage code and does not change MariaDB handler behavior.
- `write_packed_inline_insert_pages()` reserves append-buffer pages for packed
  row and append-only index-entry publication, then calls
  `write_packed_index_entry_pages()`.
- `write_packed_index_entry_pages()` encodes a new packed index-entry page with
  `encode_packed_index_entry_page()` when no compatible cached page can accept
  the next entry.
- `append_packed_index_entry_to_page()` appends later entries to compatible
  cached pages, captures undo, updates the count and used bytes, and sets the
  buffered page checksum-dirty flag.
- `validate_cached_packed_index_entry_page()` and
  `cached_packed_index_entry_page_allows_tail_append()` validate page shape and
  append-tail structure without requiring a current checksum.
- `flush_statement_append_page_buffer()` refreshes checksum-dirty append-buffer
  pages before writing them to the `.mylite` file.

## Design

- Split packed index-entry page encoding into a checksum-valid public helper
  and a checksum-deferred body that leaves the checksum field zero.
- Pass the reserved append-buffer checksum-dirty slots into
  `write_packed_index_entry_pages()`.
- For newly created packed index-entry pages, use the checksum-deferred encoder
  and mark the corresponding append-buffer dirty slot.
- Keep single-entry index pages and existing checksum-valid test helpers on the
  immediate-checksum path.

## Affected Subsystems

- Active packed inline insert writer.
- Packed index-entry append cache.
- Append-buffer checksum refresh.
- Prepared-insert checksum profiling counters.

## Compatibility Impact

No SQL-visible behavior change is intended. Index-entry bytes, row references,
duplicate filtering, and maintained-root planning inputs stay unchanged.

## Single-File And Lifecycle Impact

No new sidecars or durable format changes. The deferred checksum state is
transient append-buffer metadata; flush still publishes checksum-valid index
entry pages to the primary `.mylite` file.

## Public API And File-Format Impact

No public API, on-disk layout, checksum algorithm, or page-format version
change.

## Storage-Engine Routing Impact

No routing change. Routed durable tables that already use packed indexed
append-buffer inserts may avoid redundant hot-path checksum work.

## Binary-Size Impact

Small first-party helper split only. No dependency change.

## Tests And Verification Plan

- Add focused storage test-hook coverage that:
  - creates a new packed index-entry page through
    `write_packed_index_entry_pages()`;
  - proves the reserved append-buffer page has checksum `0` and a dirty slot;
  - proves initial encode records no zero-tail checksum;
  - flushes the append buffer and decodes the durable packed index-entry page.
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

- New packed index-entry append-buffer pages are checksum-dirty until flush.
- Append-buffer flush publishes checksum-valid packed index-entry pages.
- Prepared-insert benchmark output removes `encode_packed_index_entry_page`
  from the checksum call-site table or reduces it to callers outside the packed
  append-buffer path.
- Maintained-root planning and recovery-journal validation remain unchanged.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `318.79 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `390.17 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, producing `libmariadbd.a` at `33,980,546` bytes (`32.41 MiB`) with
  `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed twice. The structural counters were stable under unrelated host load:
  full-page checksum calls stayed at `8`, zero-tail checksum calls fell to
  `227,232`, insert-loop index-entry zero-tail calls fell to `18`, and
  `encode_packed_index_entry_page` no longer appears in the checksum call-site
  table. Maintained-root decode sites stayed at `677` total: `674` under
  `plan_maintained_index_root_inserts`, `2` under
  `validate_recovery_journal_saved_page`, and `1` under
  `read_index_leaf_run_root`.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.

## Risks And Unresolved Questions

- Any future caller using the checksum-deferred packed index-entry encoder must
  carry an equivalent checksum-dirty publication flag. This slice keeps the
  deferred path scoped to reserved append-buffer pages.
- Remaining index-leaf dirty refreshes dominate the profile and require
  separate publication-policy work.
