# Maintained Root Overflow Tail Checksum Deferral

## Problem

After maintained-root insert checksum deferral, the prepared-insert profile
still reports a small maintained-root checksum tail:

- `2` index-root zero-tail checksum calls under
  `mark_maintained_index_root_overflow_tail`;
- `2` index-root dirty-page-copy checksum refreshes from
  `promote_maintained_index_root_overflow_branch`.

The maintained-root decode sites remain planning and recovery-journal
validation gates and are intentionally out of scope.

## Source Findings

- `plan_maintained_index_root_inserts()` records planned overflow-root metadata
  and appends the root page id to the protected-page set.
- `begin_write_journal_for_statement_pages()` still validates the protected
  saved page before overflow-tail publication.
- `mark_maintained_index_root_overflow_tail()` validates the root page against
  the planned metadata, sets the overflow-tail flag and tail page id, and then
  refreshes the maintained-root checksum immediately.
- The dirty-page buffer already publishes checksum-dirty maintained-root pages
  with valid checksums at copy, direct-write fallback, or flush boundaries.

## Design

Keep planning and journal validation unchanged. Reuse the checksum-dirty
maintained-root read helper for overflow-tail marking so a dirty local root can
be copied without forcing a refresh first. Extend
`mark_maintained_index_root_overflow_tail()` with an internal
`refresh_checksum` switch. The overflow-tail writer uses no-refresh mode and
stores the marked root through `pager_write_buffered_maintained_index_page()`
with `checksum_dirty=1`.

The promotion writer also reads the root through the checksum-dirty maintained
root helper before validating it against the planned overflow-root metadata, so
promotion does not refresh a local dirty root only to run the same metadata
checks.

Direct-write fallback still refreshes the checksum before writing durable
bytes, and ordinary durable reads still validate checksums.

## Compatibility Impact

No SQL behavior, public API behavior, storage-engine routing, file-format,
checksum algorithm, or durable page image changes.

## Single-File And Lifecycle Impact

No files are introduced. Existing journal, rollback, dirty-buffer, and recovery
paths remain responsible for publication and cleanup.

## Binary Size And Dependency Impact

No new dependency. The slice only reuses existing dirty-buffer checksum
machinery for one maintained-root writer.

## Tests And Verification Plan

- Extend storage self-test coverage to prove overflow-tail marking can leave a
  root checksum-dirty and that the dirty checksum refresh path repairs it.
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
`mark_maintained_index_root_overflow_tail` as an index-root zero-tail checksum
call site, and `dirty-page-copy` reports no index-root refreshes.

Current `prepared-insert-components 1000 100000` counters:

- prepared insert step: `77.041 us/op`;
- full-page checksum calls: `2,329`;
- zero-tail checksum calls: `242,827`;
- maintained-root decodes: `1,449`;
- index-root full-page checksum calls under
  `decode_maintained_index_root_page`: `777`;
- index-root zero-tail checksum calls: `0`;
- dirty-page-copy checksum refreshes: `0`.

The remaining maintained-root decode sites are unchanged:

- `774` under `validate_recovery_journal_saved_page`;
- `674` under `plan_maintained_index_root_inserts`;
- `1` under `read_index_leaf_run_root`.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed in `309.74 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive size `33,978,938` bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed in `331.28 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

## Acceptance Criteria

- `mark_maintained_index_root_overflow_tail` no longer appears as an index-root
  zero-tail checksum call site in the prepared-insert benchmark.
- Index-root dirty-page-copy checksum refreshes drop to `0`.
- Maintained-root planning and journal validation decode sites remain visible.

## Risks

- The marked root can carry a stale checksum slot while it is resident in the
  dirty buffer. The writer still validates planned root metadata before marking
  the page, and dirty-buffer publication paths refresh the checksum before
  durable write.
