# Maintained Root Insert Checksum Deferral

## Problem

After the planned overflow-root state slice, the prepared-insert benchmark still
reports `243,497` zero-tail checksum calls. The remaining maintained-root decode
sites are planning and recovery-journal validation gates that must stay in
place, but the insert writer still refreshes maintained-root checksums inside
`insert_maintained_index_root_entry()`.

The current `prepared-insert-components 1000 100000` counters before this slice
show:

- prepared insert step: `78.827 us/op`;
- full-page checksum calls: `3,001`;
- zero-tail checksum calls: `243,497`;
- maintained-root decodes: `1,449`;
- `668` index-root zero-tail checksum calls under
  `insert_maintained_index_root_entry`.

## Source Findings

- This slice is first-party MyLite storage work only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`,
  `docs/architecture/storage.md`, and `docs/ROADMAP.md`.
- `plan_maintained_index_root_inserts()` still decodes maintained roots and
  records protected root pages before the writer mutates them.
- `begin_write_journal_for_statement_pages()` still validates the protected
  page images through `validate_recovery_journal_saved_page()`.
- `validate_planned_maintained_index_root_insert_target()` validates the
  writer's root metadata, entry count, capacity, used-byte range, and adjacent
  key order without requiring another checksum scan.
- The dirty-page buffer already accepts checksum-dirty maintained index root
  pages and refreshes their checksum before generic dirty-page reads, direct
  fallback writes, or flush publication.

## Design

Keep planning and journal validation unchanged. Extend
`insert_maintained_index_root_entry()` with an internal `refresh_checksum`
switch. Existing decode-based callers keep the old behavior. The planned insert
writer uses the no-refresh mode, writes the root page to the dirty-page buffer
with `checksum_dirty=1`, and relies on the existing dirty-buffer publication
paths to refresh the checksum.

To avoid turning each subsequent planned root insert into an immediate
dirty-buffer checksum refresh, the maintained-root insert writer reads prior
dirty root bytes through a writer-local dirty-buffer copy path that does not
refresh checksum-dirty pages. The copied page is still checked by
`validate_planned_maintained_index_root_insert_target()` before the next insert
is applied.

The next statement's planning pass can also see those local checksum-dirty root
pages through a parent dirty buffer. For those local pages only, planning copies
the dirty-buffer bytes without refreshing the stale checksum slot and decodes
the maintained root with checksum validation suppressed while keeping the same
root metadata, capacity, row-reference, and key-order validation. Durable file
reads and recovery-journal saved-page validation keep checksum validation.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata layout, file-format bytes, checksum algorithm, or durable
write policy changes. Durable page images are still published with valid
checksums.

## Single-File And Lifecycle Impact

No files are introduced. Statement rollback, dirty-page undo capture, journal
protection, dirty-buffer pressure flushing, and close/recovery behavior remain
on the existing paths.

## Binary Size And Dependency Impact

No new dependency. The slice adds a small internal writer helper and one focused
self-test.

## Tests And Verification Plan

- Add a storage self-test proving planned maintained-root insert mutation can
  skip the immediate checksum refresh and later refresh through the dirty-page
  checksum path.
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
`insert_maintained_index_root_entry` as an index-root zero-tail checksum call
site, and the local dirty-buffer planning copies no longer force the same
refresh under `dirty-page-copy`.

Current `prepared-insert-components 1000 100000` counters:

- prepared insert step: `80.021 us/op`;
- full-page checksum calls: `2,333`;
- zero-tail checksum calls: `242,831`;
- maintained-root decodes: `1,449`;
- index-root full-page checksum calls under
  `decode_maintained_index_root_page`: `781`;
- index-root zero-tail checksum calls: `4`;
- index-root dirty-page-copy checksum refreshes: `2`.

The remaining maintained-root decode sites are unchanged:

- `774` under `validate_recovery_journal_saved_page`;
- `674` under `plan_maintained_index_root_inserts`;
- `1` under `read_index_leaf_run_root`.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed in `467.02 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive size `33,975,122` bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed in `359.93 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

## Acceptance Criteria

- `insert_maintained_index_root_entry` no longer appears as an index-root
  zero-tail checksum call site in the prepared-insert benchmark.
- Planning and recovery-journal maintained-root decode sites remain visible.
- Storage and storage-smoke verification passes.

## Risks

- A dirty root copied for a subsequent planned insert can carry a stale checksum
  slot until publication. The writer continues to validate planned root
  metadata and adjacent key order before every mutation, and the dirty-buffer
  publication paths refresh the checksum before durable write.
