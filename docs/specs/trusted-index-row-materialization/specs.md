# Trusted Index Row Materialization

## Problem

Exact index cursor construction already filters hidden and replaced row ids
before handing row ids to the MariaDB handler. Cursor materialization still
called the general `mylite_storage_read_row()` API for each selected row, and
that API scans later row-state pages again to prove the row is live. Secondary
exact reads that return many rows therefore pay repeated visibility scans after
the storage exact-index path has already done that work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  builds handler cursors from storage-filtered exact or broad index entrysets.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_exact_index_entries()`
  and `mylite_storage_read_index_entries()` only return live row ids.
- `ha_mylite::read_index_cursor_row()` then re-read each selected row through
  `mylite_storage_read_row()`, which validates liveness by scanning later
  row-state pages for each row.

## Design

- Keep `mylite_storage_read_row()` unchanged for general callers. It still
  validates row visibility by scanning later row-state pages.
- Add `mylite_storage_read_indexed_row()` for handler index cursors. It:
  - opens the same primary file,
  - validates the requested row id against the current header bounds,
  - reads and validates the row page and owning table id,
  - copies the row payload,
  - skips the later row-state visibility scan because the cursor's row ids came
    from storage-filtered index entrysets.
- Route durable `ha_mylite::read_index_cursor_row()` through the indexed-row
  read API. Runtime-volatile MEMORY/HEAP cursors keep their existing volatile
  row lookup.

## Compatibility Impact

No SQL-visible behavior change is intended. The new read path is only used for
row ids produced by MyLite's own live index-entry APIs during one handler cursor
lifetime. Stale direct row-id callers must keep using `mylite_storage_read_row()`.

## Single-File And Lifecycle Impact

No file-format or file-lifecycle change. The optimization removes redundant
reads; it does not add companions or change recovery behavior.

## Public API And File-Format Impact

The first-party storage API gains `mylite_storage_read_indexed_row()`. This is
an internal storage/handler contract, not a public `libmylite` SQL API change.
No on-disk format changes.

## Storage-Engine Routing Impact

Durable MyLite-routed tables benefit for primary, unique, and secondary index
cursors whose row ids are produced by storage-filtered index entrysets. Volatile
MEMORY/HEAP tables remain process-local and unchanged.

## Tests And Verification

- Add storage unit coverage proving `mylite_storage_read_indexed_row()` can
  materialize a row id returned through index lookup.
- Run storage unit tests, storage-engine compatibility coverage, the local
  performance baseline, formatting, and whitespace checks.

## Local Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build build/mariadb-mylite-storage-smoke --target mylite_se mysqlserver`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1`
  - Direct secondary-index exact selects: `54.345 ms`.
  - Prepared secondary-index exact selects: `56.586 ms`.
  - Direct published-leaf secondary-index exact selects: `43.579 ms`.
  - Prepared published-leaf secondary-index exact selects: `44.468 ms`.
- `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc`
- `git diff --check`

## Acceptance Criteria

- Handler index cursors over durable MyLite tables materialize already-filtered
  row ids without a second row-state visibility scan.
- General row-id reads keep their visibility validation.
- Existing storage and storage-engine compatibility tests pass.
- The local performance baseline records a material improvement in exact
  secondary reads that return many rows.

## Risks

- The API relies on handler callers using only row ids returned by MyLite's
  live index-entry APIs. Future call sites must not use it for arbitrary row ids.
- This does not address the cost of building exact index entrysets. Navigable
  maintained index pages are still needed for SQLite-like point-read
  performance at larger database sizes.
