# Active Rewrite Header Publish Skip

## Problem

Cached active row-update rewrites mutate already-buffered row and index-entry
pages in place. They intentionally keep the row id and `header.page_count`
unchanged. After a successful rewrite, `update_row_with_index_entries()` still
publishes the unchanged header through `publish_header_for_statement()` and then
finishes a no-op write journal.

That work is redundant for the active-rewrite path and remains visible in the
prepared point-update sample under the storage update frame.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage in
  `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` only succeeds when an active append buffer
  already contains the row page, replacement row-state page, and any changed
  index-entry pages.
- On success, `update_row_with_index_entries()` sets
  `position.row_page_id = row_id` and `next_page_id = header.page_count`.
- `publish_header_for_statement()` updates `statement->current_header`, marks
  it dirty, and clears catalog-root caches when catalog identity changes. None
  of that is needed when the header bytes are unchanged.
- `finish_write_journal_for_statement()` returns immediately for active
  statements; active rewrites require an active statement.

## Design

Skip header publication and write-journal finishing when
`used_active_update_rewrite` is true.

Keep the existing publication path for:

- inline update append runs,
- fallback row/state/index page appends,
- maintained-root update plans,
- any update path that advances `header.page_count`.

The active rewrite still captures buffered-page undo before mutation and still
runs the existing cache maintenance after success.

## Affected Subsystems

- MyLite storage active append-buffer update rewrites.
- Prepared-update storage performance path.

No MariaDB SQL, handler, catalog, public API, or wire-protocol code is changed.

## Compatibility Impact

No SQL behavior or file-format behavior changes. Active statement visibility is
unchanged because the mutated pages are already in the active append buffer and
the header identity is unchanged.

Rollback behavior is unchanged because buffered-page undo capture happens
before page mutation and does not depend on republishing an identical header.

## Single-File And Embedded Lifecycle Impact

No durable state, companion file, lock, recovery-journal format, or embedded
lifecycle change.

## Public API Or File-Format Impact

No public `libmylite` API, first-party storage API, or `.mylite` file-format
change.

## Binary-Size Impact

One narrow branch change in existing first-party C code. No dependency change.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run storage unit and focused embedded storage-engine tests.
- Run full storage-smoke CTest.
- Rebuild the MariaDB storage-smoke embedded archive and relink the embedded
  smoke binaries.
- Run prepared-update component and full prepared-update baselines.
- Sample a focused prepared-update run and confirm the active rewrite path no
  longer samples `publish_header_for_statement()`.
- Run `git diff --check` and `git clang-format --diff` on
  `packages/mylite-storage/src/storage.c`.

## Acceptance Criteria

- Successful active rewrites skip unchanged header publication.
- Update paths that can append pages keep publishing the advanced header.
- Existing storage rollback and embedded storage-engine tests pass.
- Prepared-update sample evidence shows the header publication frame removed or
  materially reduced under the active rewrite path.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: no
  changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed before and after the MariaDB storage-smoke archive relink.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build build libmariadbd.a`: passed with existing
  no-symbol archive warnings.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`: prepared update step
  measured `2.227 us/op` in one focused run.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 10000 1000000`: prepared updates measured
  `2.271 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=storage-row-update-components 10000 1000000`: storage update mutation
  measured `0.322 us/op`.
- A one-second macOS `sample` run over prepared update components showed no
  `publish_header_for_statement()` or `finish_write_journal_for_statement()`
  frame under the active rewrite path.

## Risks And Unresolved Questions

- The optimization relies on `used_active_update_rewrite` remaining true only
  when the row mutation did not change the active header. If future active
  rewrite paths update maintained roots or other header-visible state, they
  must leave this skip path disabled until reviewed.
