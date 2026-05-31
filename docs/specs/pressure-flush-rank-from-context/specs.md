# Pressure Flush Rank From Context

## Problem

Prepared-insert profiling still rescans the dirty-page buffer to classify a
pressure victim's page-id rank even when pressure selection has already scanned
the buffer and computed both the victim index and maximum resident leaf page id.
The same pressure flush then records page-id rank again for the flush table,
creating duplicate test-hook rank scans on buffer-limit pressure rows.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`.
- `dirty_page_buffer_pressure_complete_flush_context()` already derives
  `flush_index`, `max_leaf_page_id`, and `has_leaf_page_id` in one scan.
- `dirty_page_buffer_merge_pressure_context()` stores that context in
  `mylite_storage_dirty_page_buffer_merge_pressure_context_plan` for planned
  merge fallback pressure stores.
- `record_dirty_page_buffer_merge_fallback_leaf_pressure_victim()` currently
  calls `dirty_page_buffer_flush_leaf_page_id_rank_for_entry()`, which scans the
  dirty buffer for the max leaf page id again.
- `flush_dirty_page_buffer_entry()` also records flush page-id rank by calling
  `record_dirty_page_buffer_flush_leaf_page_id_rank_facts()`, which uses the
  same rank scan.
- The remaining prepared-insert maintained-root decodes are still planning,
  root-read, and journal-validation sites and are not part of this slice.

## Design

Carry an optional test-hook pressure flush context into the two rank recorders:

- derive the victim page-id rank from `max_leaf_page_id` when a valid context is
  available;
- keep the existing scan fallback for statement-commit flushes, direct test
  calls, invalid contexts, and non-pressure paths;
- route planned merge fallback stores with their already-built pressure context;
- for unplanned buffer-limit stores in test-hook builds, use
  `dirty_page_buffer_pressure_complete_flush_context()` as the pressure selector
  source so the selected index and rank evidence come from the same scan.

Production builds continue to use the existing pressure selector and storage
publication paths. The test-hook context only changes profiling attribution
work, not flush order or durable bytes.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, or compatibility support status changes. This is
`MYLITE_STORAGE_TEST_HOOKS` profiling-source work only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page publication, checksum refresh,
journaling, rollback, and nested-statement merge behavior stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds should not gain the extra rank
context plumbing.

## Tests And Verification

- Existing storage self-tests cover buffer-limit and statement-commit rank rows,
  including non-leaf `invalid` rank preservation.
- Verified on 2026-05-31:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
- The dev storage CTest pass completed in `313.14 sec`; the storage-smoke CTest
  pass completed in `328.37 sec`.
- The MariaDB static smoke build produced
  `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` at `33,989,146`
  bytes with `478` archive members.
- The storage-smoke prepared-insert benchmark sample reported `76.299 us/op`
  under variable host load. Structural counters stayed unchanged:
  - `8` full-page checksum calls;
  - `227,063` zero-tail checksum calls;
  - `677` maintained-root decodes from `read_index_leaf_run_root`,
    `plan_maintained_index_root_inserts`, and
    `validate_recovery_journal_saved_page`;
  - flush leaf page-id rank rows of `21,030` buffer-limit `non-max-leaf-page-id`,
    `1` buffer-limit `max-leaf-page-id`, `1` statement-commit
    `max-leaf-page-id`, and `2` statement-commit `invalid`;
  - `21,031` dirty leaf pressure admissions;
  - `66,144` merge direct writes;
  - `87,176` index-leaf dirty refreshes;
  - `31,938` pressure-context builds and `19,053` planned stores.

## Acceptance Criteria

- Prepared-insert flush page-id rank and rank/fill-band counters are unchanged,
  including statement-commit non-leaf `invalid` rows.
- Merge fallback pressure victim rank counters are unchanged.
- Prepared-insert checksum and maintained-root decode counters are unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- Pressure contexts are evidence for the current resident dirty buffer, not a
  durable index-ordering proof. The carried rank must only replace equivalent
  same-buffer test-hook classification.
