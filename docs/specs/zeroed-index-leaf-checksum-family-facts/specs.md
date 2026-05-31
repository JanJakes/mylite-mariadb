# Zeroed Index Leaf Checksum Family Facts

## Problem

The prepared-insert checksum call-site table still reports `25,572`
zero-tail index-leaf checksum calls from `encode_zeroed_index_leaf_page` in
the insert loop. The encoder writes an index-leaf page header before computing
the checksum, so the page family is already known by construction, but
test-hook checksum attribution reparses the page header for every call.

This is redundant profiling work only. The index-leaf checksum must still be
computed and written for the encoded page image.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  checksum call-site counters.
- `encode_zeroed_index_leaf_page()` writes
  `MYLITE_STORAGE_FORMAT_INDEX_PAGE_TYPE_TABLE_INDEX_LEAF` before calling the
  zero-tail checksum helper over
  `MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET`.
- The current prepared-insert profile reports `25,572`
  `encode_zeroed_index_leaf_page` zero-tail checksum calls, split into
  `24,796` pages from `prepare_zeroed_index_leaf_range_pages`, `772` from
  `prepare_index_leaf_split_pages`, and `4` from
  `prepare_index_branch_snapshot_pages_with_order`.
- Earlier checksum-family slices added a test-hook
  `checksum_page_zero_tail_known_family_at_site()` helper that records
  checksum family and call-site counters from a caller-supplied known page
  family while preserving the checksum bytes.

## Design

Use the known-family zero-tail checksum helper in
`encode_zeroed_index_leaf_page()` after the function has written the index-leaf
page header and before storing the checksum. Keep generic
`checksum_page_zero_tail()` callers on parser-backed attribution.

Do not change page bytes, checksum offsets, used-size calculation, index-leaf
entry ordering, branch fence propagation, dirty-buffer publication,
maintained-root planning, journal validation, or non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
metadata, transaction, or compatibility support status changes. This is
test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Encoded index-leaf pages still carry the same
checksum bytes before publication.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing index-leaf
checksum path.

## Tests And Verification Plan

- Extend focused index-leaf encoder self-test coverage so direct
  `encode_zeroed_index_leaf_page()` calls record the same index-leaf zero-tail
  checksum site.
- Existing storage self-tests and storage-smoke coverage exercise index-leaf
  split, range encoding, branch snapshots, commit, rollback, and recovery.
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

- `encode_zeroed_index_leaf_page` checksum call-site rows stay at `25,572`
  zero-tail index-leaf calls for the prepared-insert profile.
- Index-leaf encode-site rows, full-page checksum calls, zero-tail checksum
  calls, dirty-refresh counters, maintained-root decode sites, dirty-page
  flush counts, append-buffer flush counts, pressure-admission counts, and
  merge direct-write counts stay unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `351.80 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `401.48 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `73.758 us/op` prepared insert step under variable host load.

Current `prepared-insert-components 1000 100000` counters:

- `encode_zeroed_index_leaf_page` call-site rows: `25,572` index-leaf
  zero-tail calls;
- index-leaf encode-site rows: `24,796` from
  `prepare_zeroed_index_leaf_range_pages`, `772` from
  `prepare_index_leaf_split_pages`, and `4` from
  `prepare_index_branch_snapshot_pages_with_order`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- insert-loop index-leaf zero-tail checksum calls: `112,747`;
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

- The known family must only be used inside the leaf encoder after the header
  has been set to `TABLE_INDEX_LEAF`. Keep the parser-backed checksum path for
  generic checksum callers and future index-like callers that have not proved
  page family first.
