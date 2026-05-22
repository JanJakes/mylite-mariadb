# Active Update Deferred Journal Begin

## Problem

Prepared primary-key updates now mutate rows through a resolved active storage
statement, and steady-state samples show the durable mutation path no longer
opens the generic update-file scope. The hot path still calls
`begin_write_journal_for_statement_pages()` before it knows whether
`rewrite_active_update_pages()` can rewrite buffered active pages in place.

For successful active buffered rewrites, no durable page is written and rollback
is protected by the active statement undo list. Starting or checking the write
journal before that rewrite attempt is unnecessary work on the steady prepared
update path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `ha_mylite::direct_update_rows()` reads one exact row and calls
  `ha_mylite::update_row()`, which now reaches
  `mylite_storage_update_row_with_index_entry_changes_in_statement()` for the
  prepared update benchmark.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  currently begins write-journal protection before attempting the active
  buffered rewrite.
- `rewrite_active_update_pages()` only mutates buffered active pages after
  capturing active statement undo ranges.
- The existing non-rewrite paths still need the current journal behavior before
  inline append pages, overflow payload pages, maintained index root updates,
  header publication, and durable writes.

## Design

- Attempt `rewrite_active_update_pages()` before beginning the write journal
  when no maintained index-root update plan is needed.
- If the active rewrite succeeds, skip the journal begin and the existing
  header publication / journal finish path.
- If the active rewrite is not used, begin the write journal immediately before
  the existing inline append or general row-update write paths.
- Preserve the existing protected-page set for maintained index-root updates
  and all raw filename callers.

## Affected Subsystems

- MyLite storage durable row-update internals.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL behavior, row visibility, rollback, duplicate-key, foreign-key, warning,
affected-row, or file-format behavior should change.

## Single-File And Embedded Lifecycle Impact

No file-format or companion-file lifecycle change. The slice only avoids an
unneeded journal begin on active buffered rewrites that do not publish durable
pages.

## Tests And Verification Plan

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full `storage-smoke-dev` preset.
- Rebuild the storage-smoke MariaDB embedded archive.
- Run `prepared-update-components` and capture a delayed steady-loop sample.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10 tests.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed.
  `libmariadbd.a` measured 21,188,224 bytes, 20.21 MiB, with 481 archive
  members.
- `build/storage-smoke-dev/packages/mylite-perf/mylite_perf_baseline
  prepared-update-components 10000 1000000`: passed. The delayed steady-loop
  sample captured `rewrite_active_update_pages()` under
  `mylite_storage_update_row_with_index_entry_changes_in_statement()` without
  `begin_write_journal_for_statement_pages()` in the sampled active rewrite
  path. The benchmark reported bind `0.022 us/op`, step `1.824 us/op`, and
  reset `0.023 us/op` on this machine.

## Acceptance Criteria

- Successful active buffered row-update rewrites do not call
  `begin_write_journal_for_statement_pages()` before mutation.
- Inline append, maintained-root, overflow, and raw filename update paths still
  begin journal protection before durable writes.
- Existing storage and embedded storage-engine tests pass.

## Risks And Unresolved Questions

- The begin-journal call must move only after the active rewrite attempt, not
  after any path that writes new pages or maintained roots.
