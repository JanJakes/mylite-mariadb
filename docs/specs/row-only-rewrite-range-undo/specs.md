# Row-Only Rewrite Range Undo

## Problem

Prepared primary-key update profiling still shows active row rewrites and their
buffered-page rollback preimage copies under `rewrite_active_update_pages()`.
The common benchmark shape updates a non-indexed fixed-width value:
`UPDATE perf_rows SET value = value + 1 WHERE id = ?`.

After the first in-transaction update, later updates to the same row can rewrite
the still-buffered replacement row page in place. For stable-size row-only
updates, the row page header, row id, table id, record count, overflow root, and
payload length remain unchanged, but the current undo helper still copies the
whole meaningful row-page prefix from byte `0` through the payload.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file changes are
  required.
- `rewrite_active_update_pages()` dispatches row-only updates to
  `rewrite_active_row_only_update_page()` when the handler-provided
  changed-index vector has zero changed entries.
- `rewrite_active_row_only_update_page()` validates the row page and row-state
  page before marking a reusable row-only shape, then captures a buffered-page
  undo before mutating the active append-buffer page.
- `mylite_storage_buffered_page_undo` currently stores a page id, dirty-checksum
  state, used byte count, and a fixed page preimage buffer. Rollback restores a
  partial prefix by replacing the buffered page with a zeroed page plus that
  prefix.
- The row checksum field is before the row payload. A read can refresh a dirty
  buffered page checksum between mutation and rollback, so a compact row-payload
  undo must also restore the old checksum slot when the prior page was clean.

## Design

- Extend buffered-page undo entries with a byte offset. Existing callers keep
  offset `0` and retain the current prefix-restore behavior.
- Add a range undo capture helper that copies bytes from
  `page + offset` into the undo entry and records `offset` plus `used_size`.
- Teach rollback to restore non-zero-offset entries by copying the saved range
  back into the buffered append page and restoring the saved checksum-dirty
  flag, instead of replacing the whole page.
- Use the range helper only for row-only active rewrites where the current row
  size already equals the new row size.
- For that same-size row-only path, save bytes from
  `MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET` through the row payload end, then
  copy only the new payload bytes into the row page. The checksum slot is not
  rewritten during the update; the dirty flag continues to force refresh when
  needed.
- Keep the existing prefix undo and full row-page rewrite for row-size changes,
  overflow rows, changed-index updates, and all generic buffered-page undo
  callers.

## Affected Subsystems

- MyLite active append-buffer row rewrite path.
- Statement/savepoint rollback for buffered append pages.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, metadata, or
file-format behavior changes. The optimization is selected only after existing
row-only active rewrite validation has proven the buffered row/state shape.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only reduces transient in-memory rollback preimage copying for
active buffered row pages.

## Public API And File-Format Impact

No public API, internal storage API, or durable file-format change.

## Binary-Size And Dependency Impact

Small first-party C helper split. No dependency or build-profile change.

## Tests And Verification

- Add storage coverage for same-size row-only active rewrite rollback after a
  read refreshes the dirty buffered page checksum.
- Add storage coverage for a same-savepoint same-size rewrite followed by a
  size-changing row-only rewrite so the compact range undo upgrades to the
  conservative prefix undo without losing the original row bytes.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Rebuild the MariaDB storage-smoke archive when `storage.c` changes, then
  relink the embedded storage-engine and benchmark targets.
- Run:
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 1000 1000000`
  - a focused sampled prepared-update run when machine load is low
  - `git diff --check`
  - `git clang-format --diff`

## Acceptance Criteria

- Same-size row-only active rewrites capture a checksum-plus-payload range undo
  instead of a row-page prefix undo.
- Rollback still restores correct row bytes after a read refreshes the dirty
  buffered row checksum between update and rollback.
- Row-size-changing active rewrites keep the existing prefix undo path.
- Existing storage, embedded storage-engine, transaction, and rollback tests
  pass.
- Focused benchmark/profile evidence records whether active rewrite copy cost
  moves down or exposes the next bottleneck.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  build libmariadbd.a` passed with the pre-existing libtool no-symbol warnings.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed after the
  MariaDB archive rebuild.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` recorded prepared primary-key updates
  at `2.293`, `2.287`, `2.319`, and `2.288 us/op` across four runs.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 10000 1000000` recorded prepared primary-key updates
  at `2.610` and `2.620 us/op` across two longer runs.
- macOS `sample` did not produce a usable profile in this run: the benchmark
  process exited, but `sample` remained stuck until killed. No profile claim is
  made from that attempt.
- `git diff --check` and `git clang-format --diff` passed.

## Risks And Unresolved Questions

- Compact range undo relies on the row-only same-size precondition. Broader row
  rewrites must continue to use the conservative prefix restore path, including
  upgrading an earlier compact undo entry when a later same-savepoint rewrite
  changes the row size.
- This reduces only one copy span in the hot path. True SQLite-like write
  throughput still needs navigable indexes and a pager/WAL design rather than
  append-history rewrites.
