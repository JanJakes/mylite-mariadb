# Flush Rank Occupancy Reuse

## Problem

Prepared-insert profiling still parses flushed index-leaf occupancy twice for
the same dirty-buffer entry. `record_dirty_page_buffer_flush_page()` already
reads fixed-width leaf metadata for flush shape, fill-band, free-slot, and
replacement-state counters. The later flush page-id rank/fill-band recorder
re-reads the same leaf metadata only to join the already-recorded rank with the
leaf fill band.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`.
- `record_dirty_page_buffer_flush_page()` computes
  `mylite_storage_test_index_leaf_occupancy` and passes it to
  `record_dirty_page_buffer_flush_leaf_occupancy()`.
- `record_dirty_page_buffer_flush_leaf_page_id_rank_facts()` currently computes
  another `mylite_storage_test_index_leaf_occupancy` for the same flushed entry.
- The latest prepared-insert benchmark reports `21,031` buffer-limit
  index-leaf flushes, `1` statement-commit index-leaf flush, and `2`
  statement-commit index-branch flushes. The two branch rows must still record
  `invalid` rank without trying to reuse leaf occupancy.
- The remaining maintained-root decodes are planning, root-read, and
  recovery-journal validation sites and are not part of this slice.

## Design

Add a test-hook-only flush-entry facts helper that computes occupancy once per
flushed dirty-buffer entry, records the existing flush page counters, then
passes the same occupancy facts into the rank/fill-band recorder.

Keep the current standalone `record_dirty_page_buffer_flush_page()` behavior
for focused tests and any direct caller that only wants flush page counters.
Keep rank fallback behavior intact: when no precomputed occupancy is supplied,
the rank recorder computes it exactly as before.

Production builds remain unchanged because the new occupancy plumbing is
compiled only under `MYLITE_STORAGE_TEST_HOOKS`.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, or compatibility support status changes. This is
`MYLITE_STORAGE_TEST_HOOKS` profiling-source work only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page publication, checksum refresh,
journaling, rollback, and statement commit behavior stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds should not gain the helper.

## Tests And Verification

- Existing storage self-tests cover flush leaf occupancy counters, flush
  page-id rank counters, rank/fill-band counters, and statement-commit
  non-leaf `invalid` rank rows.
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
- The dev storage CTest pass completed in `307.03 sec`; the storage-smoke CTest
  pass completed in `336.56 sec`.
- The MariaDB static smoke build produced
  `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` at `33,989,146`
  bytes with `478` archive members.
- The storage-smoke prepared-insert benchmark sample reported `76.433 us/op`.
  Structural counters stayed unchanged:
  - `8` full-page checksum calls;
  - `227,063` zero-tail checksum calls;
  - `677` maintained-root decodes from `read_index_leaf_run_root`,
    `plan_maintained_index_root_inserts`, and
    `validate_recovery_journal_saved_page`;
  - `21,031` dirty leaf pressure admissions;
  - `66,144` merge direct writes;
  - `87,176` index-leaf dirty refreshes;
  - `31,938` pressure-context builds and `19,053` planned stores;
  - unchanged flush leaf page-id rank rows, including `21,030` buffer-limit
    `non-max-leaf-page-id`, `1` buffer-limit `max-leaf-page-id`, `1`
    statement-commit `max-leaf-page-id`, and `2` statement-commit `invalid`.

## Acceptance Criteria

- Prepared-insert flush page-id rank and rank/fill-band counters are unchanged,
  including statement-commit non-leaf `invalid` rows.
- Prepared-insert checksum, maintained-root decode, pressure-admission, merge
  direct-write, and dirty-refresh counters are unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- The shared occupancy facts describe only the current dirty-buffer entry. Do
  not carry them across mutation or publication boundaries.
