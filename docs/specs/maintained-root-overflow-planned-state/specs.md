# Maintained Root Overflow Planned State

## Problem

The prepared-insert profile after planned maintained-root insert positions still
reports `1,461` checksum-validating maintained-root decodes. Planning and
recovery-journal saved-page validation account for the dominant `1,448` decodes
and are the required pre-write validation gates. The remaining writer-side
duplicates are smaller but removable:

- `6` decodes under `mark_maintained_index_root_overflow_tail`;
- `6` decodes under `promote_maintained_index_root_overflow_branch`.

Both operate on maintained roots that `plan_maintained_index_root_inserts()`
has already decoded and recorded in the journal protected-page set. The write
journal then validates the saved page before root-tail marking or promotion can
publish changes.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code and docs only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`,
  `docs/architecture/storage.md`, and `docs/ROADMAP.md`.
- `plan_maintained_index_root_inserts()` decodes full single-page maintained
  roots, confirms table/index/key shape, and appends the root page id to the
  journal protected-page set before the write phase.
- `begin_write_journal_for_statement_pages()` validates each protected page
  through `validate_recovery_journal_saved_page()`.
- `mark_maintained_index_root_overflow_tail()` decodes the same root only to
  confirm metadata and learn the existing flags/used byte count before setting
  the overflow-tail flag.
- `promote_maintained_index_root_overflow_branch()` decodes the same root only
  to recover metadata and root entries before merging the append tail into a
  branch snapshot.
- `open_existing_file_for_update_scope()` holds the update file under an
  exclusive lock for this insert path, so another writer cannot mutate the
  planned root between planning, journal validation, and the writer phase.

## Design

Extend maintained-index insert planning with a small overflow-root plan record.
For each full maintained root selected for append-tail overflow, carry forward:

- root page id, table id, index number, key size, entry count, used bytes,
  flags, and existing overflow-tail page id;
- a copied entryset for the root cells decoded during planning.

The overflow-tail marker no longer decodes the root page. It skips roots that
already had an overflow tail in the planning decode. For newly tailed roots it
reads the page, performs lightweight drift checks against the planned metadata,
sets the tail flag and tail page id, and refreshes the checksum.

The promotion writer no longer decodes the root page. It reuses the planned
root entryset, appends live tail entries from the effective tail page, and
builds the same branch snapshot when the merged entryset still exceeds
single-root capacity. A lightweight root-page metadata check still runs before
promotion to catch unexpected writer-phase drift without repeating the
full-page checksum scan.

Planning decode and journal saved-page validation remain unchanged.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, checksum algorithm, or write
policy changes. The same root-tail and branch-promotion durable page images are
published.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, dirty-buffer pressure,
statement commit, and embedded lifecycle behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. The internal insert plan carries copied root entries only
for overflow roots, bounded by `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`.

## Tests And Verification Plan

- Add a storage self-test that appends a planned overflow root, marks its tail,
  runs promotion from the planned entryset, and asserts the writer path records
  no maintained-root decode sites.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Benchmark Evidence

After this slice, the prepared-insert benchmark no longer reports
maintained-root decodes under `mark_maintained_index_root_overflow_tail` or
`promote_maintained_index_root_overflow_branch`.

Current `prepared-insert-components 1000 100000` counters:

- prepared insert step: `81.209 us/op`;
- full-page checksum calls: `3,001`;
- zero-tail checksum calls: `243,497`;
- maintained-root decodes: `1,449`.

The remaining maintained-root decode sites are:

- `774` under `validate_recovery_journal_saved_page`;
- `674` under `plan_maintained_index_root_inserts`;
- `1` under `read_index_leaf_run_root`.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed in `307.44 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive size `33,975,818` bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed in `333.08 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

## Acceptance Criteria

- Prepared-insert benchmark output no longer reports maintained-root decodes
  under `mark_maintained_index_root_overflow_tail` or
  `promote_maintained_index_root_overflow_branch`.
- Planning and recovery-journal saved-page validation decode counts remain
  visible.
- Existing storage routing, rollback, dirty-buffer publication, journal
  recovery, and checksum validation behavior remain covered by the storage and
  storage-smoke tests.

## Risks

- The promotion writer now relies on copied root entries from the planning
  decode. Future changes that can mutate overflow roots between planning and
  promotion must either update that planned state or restore a writer-side
  decode.
- The slice intentionally does not remove the `674` planning decodes or `774`
  journal-validation decodes because those are the current safety gates.
