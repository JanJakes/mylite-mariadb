# Merge Direct Write Refresh Family Facts

## Problem

The prepared-insert profile still records `66,144` dirty-buffer merge
direct-write pages, all checksum-dirty `index-leaf` pages. In test-hook builds,
`write_dirty_page_buffer_entry()` refreshes and classifies each checksum-dirty
direct-write page for dirty checksum refresh accounting and publication-source
accounting. Immediately after that write,
`record_dirty_page_buffer_merge_direct_write_page()` classifies the same page
family again for the merge direct-write counter.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  custom dirty-page merge direct-write counter.
- `direct_write_dirty_page_buffer_merge_entry()` copies a child dirty-buffer
  entry, writes it with publication source `merge-direct-write`, and then
  records the merge direct-write page family.
- The preceding slice made `write_dirty_page_buffer_entry()` carry the family
  computed during dirty checksum refresh into publication-source accounting.
  Merge direct-write accounting can consume the same family after a successful
  write.
- The current prepared-insert profile reports `66,144` merge direct writes,
  `87,178` dirty-page flush checksum refreshes, `8` full-page checksum calls,
  `227,063` zero-tail checksum calls, and `677` maintained-root decodes. These
  counters must remain unchanged.

## Design

Add a test-hook-only writer helper that optionally returns the page family
computed during dirty checksum refresh. Keep the existing
`write_dirty_page_buffer_entry()` API as the default wrapper for non-test builds
and callers that do not need family facts.

Change merge direct-write page recording to accept an optional precomputed page
family. `direct_write_dirty_page_buffer_merge_entry()` supplies the family from
the successful writer refresh when available; the recorder keeps the existing
page parse fallback for clean pages, direct tests, and future callers without
writer facts.

Do not change merge direct-write policy, dirty-page publication, checksum
bytes, pressure selection, journaling, validation, or fallback publication
behavior.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, or compatibility support status changes. This is
`MYLITE_STORAGE_TEST_HOOKS` profiling-source work only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Direct-write publication, recovery journals,
statement commit, rollback, and storage file bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing dirty-page
writer API.

## Tests And Verification

- Existing storage self-tests cover merge direct-write page counters and dirty
  page publication checksum-source counters.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `314.61 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `344.12 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `76.880 us/op` for the prepared insert
step. The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
checksum-dirty `index-leaf` merge direct writes, `87,176` index-leaf dirty
refreshes, `31,938` pressure-context builds, and `19,053` planned stores. The
dirty page publication rows stayed `21,031` buffer-limit `index-leaf`, `1`
statement-commit `index-leaf`, `2` statement-commit `index-branch`, and
`66,144` merge-direct-write `index-leaf`. Maintained-root decode sites stayed
`1` `read_index_leaf_run_root`, `674` `plan_maintained_index_root_inserts`,
and `2` `validate_recovery_journal_saved_page`; the checksum call-site table
kept `refresh_dirty_buffered_page_checksum`.

## Acceptance Criteria

- Merge direct-write page counters remain `66,144` checksum-dirty `index-leaf`
  pages.
- Dirty checksum refresh, dirty page publication checksum-source, checksum
  call-site, maintained-root decode, pressure-admission, and merge guard
  counters are unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- The returned writer family only describes a page whose checksum refresh
  succeeded. If a write fails or the page was already checksum-valid, merge
  direct-write accounting must keep the fallback page-family parse.
