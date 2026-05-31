# Maintained Root Checksum Family Facts

## Problem

The prepared-insert maintained-root decode table still reports `677`
maintained-root decodes. These are safety-sensitive validation decodes:
maintained-root insert planning accounts for `674`, recovery-journal saved-page
validation accounts for `2`, and root-to-leaf read conversion accounts for
`1`. This slice must not remove those decodes.

The checksum call-site table also reports `5` full-page `index-root` checksum
calls from `decode_maintained_index_root_page`. For those full-checksum
decodes, the decoder has already matched `is_maintained_index_root_page(page)`
before computing the checksum, but test-hook checksum attribution reparses the
page header to rediscover the index-root family.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  checksum call-site counters.
- `decode_maintained_index_root_page_with_checksum_state()` rejects non-root
  page families before reading root metadata and validating the checksum.
- When `checksum_dirty` is false, the decoder computes a full-page checksum
  through `checksum_page_at_site(..., "decode_maintained_index_root_page")`.
- The current prepared-insert profile reports `5`
  `decode_maintained_index_root_page` full-page checksum calls and keeps the
  maintained-root decode table at `677` total decodes:
  `read_index_leaf_run_root` `1`, `plan_maintained_index_root_inserts` `674`,
  and `validate_recovery_journal_saved_page` `2`.

## Design

Add a test-hook full-page checksum helper that records checksum family and
call-site counters from a caller-supplied known page family.

Use it only for the full-checksum branch of
`decode_maintained_index_root_page_with_checksum_state()` after the maintained
root page match. Keep the checksum-dirty branch unchanged and keep generic
`checksum_page_at_site()` callers on parser-backed attribution.

Do not change maintained-root decode validation, checksum bytes, planning,
journal saved-page validation, dirty-buffer root handling, writer-side planned
insert state, file format, or non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
metadata, transaction, or compatibility support status changes. This is
test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Maintained-root planning and recovery-journal
protected-page validation still decode and validate the same pages.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing maintained
root checksum path.

## Tests And Verification Plan

- Extend checksum-family self-test coverage for known-family full-page
  recording so aggregate and site counters are updated without page parsing.
- Existing storage self-tests and storage-smoke coverage exercise
  maintained-root planning, journal saved-page validation, commit, rollback,
  and recovery.
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

- `decode_maintained_index_root_page` checksum call-site rows stay at `5`
  full-page index-root calls for the prepared-insert profile.
- Maintained-root decode sites stay at `677` total decodes with the same
  caller/checksum-state split.
- Full-page checksum calls, zero-tail checksum calls, dirty-refresh counters,
  dirty-page flush counts, append-buffer flush counts, pressure-admission
  counts, and merge direct-write counts stay unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `320.40 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `324.64 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `68.959 us/op` prepared insert step under variable host load.

Current `prepared-insert-components 1000 100000` counters:

- `decode_maintained_index_root_page` checksum call-site rows: `5`
  full-page index-root calls;
- maintained-root decodes: `677`;
- maintained-root decode site split: `1` `read_index_leaf_run_root`, `674`
  `plan_maintained_index_root_inserts`, and `2`
  `validate_recovery_journal_saved_page`;
- maintained-root checksum-state split: `5` full checksum and `672`
  checksum-dirty;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
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

- The known family must only be used after the maintained-root page match.
  Keep the parser-backed checksum path for generic checksum callers and for
  any future root-like callers that have not proved page family first.
