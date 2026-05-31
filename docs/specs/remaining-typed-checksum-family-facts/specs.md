# Remaining Typed Checksum Family Facts

## Problem

The prepared-insert checksum call-site table still includes small typed page
paths whose callers already know the page family before computing or validating
the checksum:

- `encode_recovery_journal_header`: `1` full-page `journal` checksum call;
- `decode_index_entry_run_page`: `12` zero-tail `index-entry` checksum calls;
- `encode_index_branch_page_from_leaf_run`: `2` zero-tail `index-branch`
  checksum calls;
- `encode_row_page`: `365` zero-tail `row` checksum calls;
- `encode_header_page`: `1` zero-tail `header` checksum call;
- `decode_header_page`: `1` full-page `header` checksum call;
- `validate_catalog_page_bytes`: `1` full-page `catalog` checksum call.

These are not the remaining production bottleneck, but leaving them on generic
test-hook attribution keeps reparsing page families after typed encoders or
decoders have already established the family.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  checksum call-site counters.
- `encode_header_page()`, `encode_recovery_journal_header()`,
  `encode_row_page()`, `encode_index_branch_page()`, and
  `encode_index_branch_page_from_leaf_run()` write the page family before
  computing the checksum.
- `decode_header_page()` validates the header magic, format, page size,
  byte-order marker, and checksum algorithm before checking the checksum.
- `validate_catalog_page_bytes()` validates catalog magic, page type, format,
  page id, generation, page-count bounds, root expectations, and used bytes
  before checking the checksum.
- `decode_index_entry_run_page()` requires `is_index_entry_page(page)` before
  decoding index-entry metadata and validating the checksum.

## Design

Add test-hook wrapper macros for known-family full-page and zero-tail checksum
calls. In test-hook builds they record aggregate and call-site counters from a
caller-supplied page family. In non-test builds they compile to the existing
checksum helpers.

Use the known-family wrappers only at typed encoders and decoders after the
caller has established the family. Keep generic checksum callers and
ambiguous page detection paths on parser-backed attribution.

Do not change checksum bytes, page bytes, page validation, recovery-journal
format, append or dirty-buffer publication policy, maintained-root planning,
journal saved-page validation, or file format.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, metadata,
transaction, recovery, or compatibility support status changes. This is
test-hook-only checksum attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Header, catalog, journal, row, index-entry, and
index-branch pages keep the same validation and checksum requirements.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds use the existing checksum
helpers through macros and keep the same checksum implementation.

## Tests And Verification Plan

- Reuse checksum-family self-test coverage for known-family full-page and
  zero-tail recording.
- Existing storage self-tests and storage-smoke coverage exercise header,
  catalog, journal, row, index-entry, and branch encode/decode paths.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- The prepared-insert checksum call-site table still reports the same typed
  site rows and counts for the covered paths.
- Structural counters stay unchanged: full-page checksum calls, zero-tail
  checksum calls, dirty-refresh counters, pressure-admission counts, merge
  direct-write counts, maintained-root decodes, and planned-store counts.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Known-family wrappers must not be used in page-family probing paths before a
  caller has established the family. Keep generic checksum helpers for
  ambiguous page detection and broad validation code.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `352.71 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `340.40 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `73.032 us/op` prepared insert step under variable host load.

Current `prepared-insert-components 1000 100000` counters:

- covered typed checksum call-site rows stayed unchanged:
  `encode_recovery_journal_header` `1` journal full-page call,
  `decode_index_entry_run_page` `12` index-entry zero-tail calls,
  `encode_index_branch_page_from_leaf_run` `2` index-branch zero-tail calls,
  `encode_row_page` `365` row zero-tail calls, `encode_header_page` `1`
  header zero-tail call, `decode_header_page` `1` header full-page call, and
  `validate_catalog_page_bytes` `1` catalog full-page call;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- dirty-page-flush refreshes: `87,178`;
- append-buffer-flush refreshes: `6,849`;
- append-buffer-copy refreshes: `6`;
- total dirty checksum refreshes: `94,033`;
- buffer-limit pressure admissions: `21,031`;
- merge direct writes: `66,144`;
- index-leaf dirty refreshes: `87,176`;
- maintained-root decodes: `677`;
- pressure-context builds: `31,938`;
- planned stores: `19,053`;
- future-page relation rows: `122,388`;
- rejected below-tail candidate admissions: `121`.
