# Row Metadata Checksum Family Facts

## Problem

The prepared-insert checksum call-site table still reports `107,078`
zero-tail row checksum calls from `decode_row_page_metadata` during the
verification phase. That decoder has already matched `is_row_page(page)` before
it computes and validates the row-page checksum, but test-hook checksum
attribution still reparses the page header to rediscover the row family.

This is redundant profiling work only. The checksum validation is still the
durable row-page validation gate and must continue to run.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  checksum call-site counters.
- `decode_row_page_metadata()` first rejects non-row page families, then reads
  row-page metadata and validates the zero-tail checksum over
  `MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET`.
- The current prepared-insert profile reports `107,078`
  `decode_row_page_metadata` zero-tail checksum calls, all attributed to the
  row page family in the verification phase.
- The preceding dirty-refresh checksum-family slice added a test-hook
  `checksum_page_zero_tail_known_family_at_site()` helper that records
  checksum family and call-site counters from a caller-supplied known page
  family while preserving the checksum bytes.

## Design

Use the known-family zero-tail checksum helper in
`decode_row_page_metadata()` after the row-page match and metadata validation
have established that the page family is row.

Keep generic `checksum_page_zero_tail()` callers on the parser-backed
attribution path. Do not change row-page metadata validation, checksum
algorithm, checksum offsets, used-size calculation, row payload decoding,
dirty-buffer publication, append-buffer flush, maintained-root planning,
journal validation, or non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
metadata, transaction, or compatibility support status changes. This is
test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Row-page reads still validate checksums before
returning decoded metadata.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing row metadata
checksum path.

## Tests And Verification Plan

- Extend focused row-page self-test coverage so decoding a flushed packed row
  page records the same `decode_row_page_metadata` row zero-tail checksum site.
- Existing storage self-tests and storage-smoke coverage exercise row reads,
  append-buffer flushes, statement rollback, commit, and recovery.
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

- `decode_row_page_metadata` checksum call-site rows stay at `107,078`
  zero-tail row calls for the prepared-insert profile.
- Full-page checksum calls, zero-tail checksum calls, dirty-refresh counters,
  maintained-root decode sites, dirty-page flush counts, append-buffer flush
  counts, pressure-admission counts, and merge direct-write counts stay
  unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `310.63 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `339.48 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `78.376 us/op` prepared insert step under variable host load.

Current `prepared-insert-components 1000 100000` counters:

- `decode_row_page_metadata` call-site rows: `107,078` row zero-tail calls;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- verification-phase row zero-tail checksum calls: `107,078`;
- maintained-root decodes: `677`;
- dirty-page-flush refreshes: `87,178`;
- append-buffer-flush refreshes: `6,849`;
- append-buffer-copy refreshes: `6`;
- total dirty checksum refreshes: `94,033`;
- buffer-limit pressure admissions: `21,031`;
- merge direct writes: `66,144`;
- index-leaf dirty refreshes: `87,176`;
- pressure-context builds: `31,938`;
- planned stores: `19,053`;
- future-page relation rows: `122,388`;
- rejected below-tail candidate admissions: `121`.

## Risks

- The known family must only be used after `decode_row_page_metadata()` has
  matched a row page. Keep the parser-backed checksum path for generic
  checksum callers and any future row-like callers that have not proved page
  family first.
