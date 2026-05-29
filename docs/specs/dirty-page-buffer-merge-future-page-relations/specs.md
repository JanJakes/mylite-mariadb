# Dirty Page Buffer Merge Future-Page Relations

## Problem

The merge direct-write guard outcome table shows that the current
prepared-insert workload is dominated by dirty `index-leaf` merge entries
blocked as `future-page`. That proves they are not eligible for the existing
protected-existing-leaf direct-write path, but it does not explain whether
those pages are already logically allocated in the parent statement, resident
in an append buffer, or beyond the parent statement's current header.

Without that relationship data, the next publication-policy slice would have
to guess whether to target append-buffer merge, current-header page-count
handling, or a different dirty-buffer pressure path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- `merge_dirty_page_buffer()` is the single point where child dirty-buffer
  entries replay into their parent statement.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` currently
  classifies `entry->page_id >= parent->header.page_count` as `future-page`.
- `mylite_storage_commit_statement()` propagates a child dirty current header
  to the parent before child dirty-buffer replay, so a page may be future
  relative to the parent's stable header while still inside the parent's
  current header page count.
- The active append buffer belongs to the outer statement selected by
  `append_page_buffer_statement_for_file()` /
  `active_statement_and_append_buffer_for_file()`, while dirty maintained index
  leaf writes use the dirty page buffer.

## Design

Keep production behavior unchanged. In test-hook builds only, when a merge
entry's direct-write guard outcome is `future-page`, classify it by:

- parent current-header relation:
  - `invalid`
  - `no-current-header`
  - `within-current-header`
  - `past-current-header`
- append-buffer relation:
  - `invalid`
  - `none`
  - `parent-append-buffer`
  - `child-append-buffer`
  - `parent-and-child-append-buffer`

Record both relations by page family and checksum-dirty state. Print nonzero
rows in the prepared-insert benchmark after the existing guard outcome table.

## Compatibility Impact

No SQL syntax, public C API, handler API, storage-engine routing, metadata, or
file-format changes. The new counters are compiled only under
`MYLITE_STORAGE_TEST_HOOKS` and do not change the direct-write decision.

## Single-File And Lifecycle Impact

No files are introduced. Durable state remains in the `.mylite` file plus the
existing MyLite-owned journal lifecycle. The slice observes in-memory statement
and append-buffer state only.

## Binary Size And Dependency Impact

No new dependencies. Production builds are unchanged except for declarations
excluded by test-hook guards. Test-hook builds gain two small relation counter
tables and benchmark output.

## Tests And Verification

- Add a storage self-test that merges future-page dirty leaf entries covering
  parent-only, child-only, parent-and-child, and no append-buffer relations,
  plus within-current-header and past-current-header relations.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Evidence

VPS verification after implementation:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The final prepared-insert smoke profile reported a `74.027 us/op` prepared
insert step. The future-page relation tables reported:

- header relation: `116,672` dirty `index-leaf` rows
  `within-current-header`; and
- append relation: `116,672` dirty `index-leaf` rows with `none`.

The existing guard table still reported `116,672` dirty `index-leaf`
`future-page` rows. This shows the current workload's future-page merge leaves
are logically inside the parent statement's current page count, but they are
not resident in either statement's append buffer.

## Acceptance Criteria

- Prepared-insert benchmark output reports future-page header and append-buffer
  relation rows.
- Existing guard outcome counters still report the `future-page` entries that
  feed the new relation tables.
- The relation self-test covers all append relation names except `invalid` and
  both current-header non-invalid states relevant to the prepared-insert
  workload.
- Storage and embedded storage-engine smoke tests pass.
