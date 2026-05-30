# Maintained Root Packed Admission Validation Elision

## Problem

Prepared-insert maintained-root decode call-site counters show `674`
maintained-root checksum-validating decodes under
`maintained_index_roots_allow_packed_insert()` and another `674` under
`plan_maintained_index_root_inserts()` in the same profile. The admission helper
initializes `allow_packed_insert` to true, validates root pages, but never
changes the admission decision. The later planning pass validates the same
maintained roots before any write journal, row append, index append, or
dirty-page publication.

Keeping both passes preserves no additional write-safety invariant in the hot
prepared insert path while repeating full-page root checksum validation.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code and docs only:
  `packages/mylite-storage/src/storage.c`, `docs/architecture/storage.md`, and
  `docs/ROADMAP.md`.
- `maintained_index_roots_allow_packed_insert()` sets
  `*out_allow_packed_insert = 1`, scans catalog root records, validates
  maintained root pages, and returns without ever setting the decision to
  false.
- `plan_maintained_index_root_inserts()` runs after row-id prediction and
  before write-journal setup. It reads the same catalog root records, validates
  maintained single-page roots, validates branch roots through the branch
  decoder, records protected pages, and computes changed index-entry state.
- The write path begins only after planning succeeds and
  `begin_write_journal_for_statement_pages()` protects the planned pages.

## Design

Remove the packed-insert admission helper call and delete the helper. Keep the
existing packed-insert behavior by setting `allow_packed_insert` once table
metadata and maintained-root planning requirements are known. Maintained-root
planning remains the first validation pass that can reject corrupt roots.

The validation gate moves to the existing planning pass. If a maintained root
is corrupt, planning returns the same corruption result before any write
journal or page publication starts.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, checksum algorithm, or write
policy changes. Corrupt maintained-root detection is deferred within the same
insert call from the no-op admission scan to the planning scan, still before
mutation.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, dirty-buffer pressure,
merge direct-write policy, statement commit, and embedded lifecycle behavior
remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Removing the unused admission helper slightly reduces
first-party storage code in both test-hook and non-test-hook builds.

## Tests And Verification

- Existing storage rollback, recovery, locking, and storage-engine CTests cover
  maintained-root planning and write-journal sequencing.
- Run:
  - `git diff --check`: pass.
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: pass.
  - `cmake --build --preset dev --target mylite_storage_test`: pass.
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
    pass in `301.29 sec`.
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
    pass; embedded archive `33,972,450` bytes.
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
    pass.
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
    pass in `313.25 sec`.
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
    prepared insert step `76.625 us/op`.

## Benchmark Evidence

The prepared-insert benchmark no longer reports a
`maintained_index_roots_allow_packed_insert` decode site. Maintained-root
decodes fell from `2,803` to `2,129`, and full-page checksum calls fell from
`4,355` to `3,681` while zero-tail checksum calls stayed at `243,497`.

The remaining maintained-root decode sites are:

- `774` under `validate_recovery_journal_saved_page`;
- `674` under `plan_maintained_index_root_inserts`;
- `668` under `insert_maintained_index_root_entry`;
- `6` under `mark_maintained_index_root_overflow_tail`;
- `6` under `promote_maintained_index_root_overflow_branch`;
- `1` under `read_index_leaf_run_root`.

## Acceptance Criteria

- Prepared-insert benchmark output no longer reports maintained-root decodes
  under `maintained_index_roots_allow_packed_insert`.
- Maintained-root planning remains the pre-write validation gate.
- Existing storage routing, rollback, dirty-buffer publication, and checksum
  validation behavior remain covered by the storage and storage-smoke tests.

## Risks

- This relies on `plan_maintained_index_root_inserts()` continuing to run before
  write-journal setup for maintained-root tables. Future changes to row-id
  prediction or planning order must preserve that sequencing.
- Corrupt root detection moves later inside the same insert operation, but still
  before mutation; tests must keep covering failed-statement rollback and
  journal setup ordering.
