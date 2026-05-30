# Maintained Root Planned Insert Position

## Problem

After removing the no-op packed-admission validation pass, the prepared-insert
profile still reports `668` full-page maintained-root decodes under
`insert_maintained_index_root_entry`. Those decodes happen after
`plan_maintained_index_root_inserts()` has already decoded the same single-page
root, found that it has room, and recorded the root page in the statement
journal protected-page set. Journal setup then validates the protected page
again before any root mutation is written.

The writer still rescans the root to find the insertion slot and revalidates
the page checksum, making it the next removable duplicate on the hot insert
path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code and docs only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`,
  `docs/architecture/storage.md`, and `docs/ROADMAP.md`.
- `plan_maintained_index_root_inserts()` decodes maintained roots before
  write-journal setup, checks table/index/key metadata, verifies capacity, and
  records protected pages.
- `begin_write_journal_for_statement_pages()` writes the recovery journal after
  validating protected pages through `validate_recovery_journal_saved_page()`.
- `write_maintained_index_root_inserts()` later reads each planned root and
  calls `insert_maintained_index_root_entry()`, which decodes the root again
  and linearly scans for the insert slot.
- `open_existing_file_for_update_scope()` holds the update file under an
  exclusive lock for this statement path, so the planned root page cannot be
  modified by another writer between planning and writing.

## Design

Extend single-page maintained-root insert plan entries with the planned root
entry count and insertion index computed during the existing planning decode.
The writer uses those values to insert directly into the planned slot.

The writer keeps lightweight drift checks before mutation:

- root magic/type, page id, table id, index number, key size, entry count, and
  used bytes must still match the planned single-page root shape;
- the planned insertion index must be within the planned entry count;
- the immediate previous and next cells, when present, must still order around
  the inserted key and row id.

Full-page checksum validation remains in the planning decode and journal page
validation. The writer no longer performs a third full-root decode for the same
single-page root.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, checksum algorithm, or write
policy changes. The planned-position writer preserves ordered root entries and
continues to refresh the root checksum after mutation.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, dirty-buffer pressure,
statement commit, and embedded lifecycle behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. The plan entry grows by two `size_t` fields for the
single-page maintained-root insert path.

## Tests And Verification

- Add a storage self-test that computes a planned maintained-root insertion
  position, writes through the planned-position helper, verifies the updated
  root decodes correctly, and asserts that the writer did not record a
  maintained-root decode site.
- Run:
  - `git diff --check`: pass.
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: pass.
  - `cmake --build --preset dev --target mylite_storage_test`: pass.
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
    pass in `311.75 sec`.
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
    pass; embedded archive `33,973,514` bytes.
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
    pass.
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
    pass in `315.51 sec`.
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
    prepared insert step `76.925 us/op`.

## Benchmark Evidence

The prepared-insert benchmark no longer reports maintained-root decodes under
`insert_maintained_index_root_entry`. Maintained-root decodes fell from
`2,129` to `1,461`, and full-page checksum calls fell from `3,681` to
`3,013`. Zero-tail checksum calls stayed at `243,497`; the planned writer still
refreshes `668` root checksums after insertion.

The remaining maintained-root decode sites are:

- `774` under `validate_recovery_journal_saved_page`;
- `674` under `plan_maintained_index_root_inserts`;
- `6` under `mark_maintained_index_root_overflow_tail`;
- `6` under `promote_maintained_index_root_overflow_branch`;
- `1` under `read_index_leaf_run_root`.

## Acceptance Criteria

- Prepared-insert benchmark output no longer reports maintained-root decodes
  under `insert_maintained_index_root_entry`.
- The prepared-insert full-page checksum total drops by the former
  single-page root insertion decode count, subject to normal benchmark shape
  drift.
- Existing storage routing, rollback, dirty-buffer publication, journal
  recovery, and checksum validation behavior remain covered by the storage and
  storage-smoke tests.

## Risks

- This relies on the existing exclusive update scope and the sequencing that
  planning and journal validation both complete before root mutation.
- Future changes that allow another writer to mutate a planned root between
  planning and writing must restore a full write-time decode or carry stronger
  page-version validation into the plan.
