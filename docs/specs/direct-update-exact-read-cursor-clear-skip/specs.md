# Direct Update Exact Read Cursor Clear Skip

## Problem

The direct-update path calls
`ha_mylite::read_exact_unique_index_row_into()` only to fetch the matched row id
and row image. It passes `MYLITE_EXACT_UNIQUE_SKIP_CURSOR_STATE`, so the helper
must not publish a one-row handler cursor. The helper still clears any existing
index cursor state before the storage lookup, which is unnecessary work for
accepted prepared direct updates.

This is not the main prepared-update performance wall, but it sits in the hot
exact-key update path and has a narrow correctness boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  calls `read_exact_unique_index_row_into()` with
  `MYLITE_EXACT_UNIQUE_SKIP_CURSOR_STATE`.
- `ha_mylite::index_read_idx_map()` calls the same helper with
  `MYLITE_EXACT_UNIQUE_PUBLISH_CURSOR_STATE`, where clearing stale cursor state
  before publishing the new exact-key cursor remains correct.
- `ha_mylite::read_exact_unique_index_row_into()` currently calls
  `clear_index_cursor()` before it knows whether cursor state will be
  published.

## Design

Move the cursor cleanup behind the `publish_cursor_state` branch:

- direct updates that skip cursor state no longer clear unrelated index cursor
  buffers before the storage lookup;
- ordinary exact indexed reads that publish cursor state still clear existing
  cursor state before reading and publishing the one-row continuation state;
- no storage API, SQL behavior, cursor continuation, or row visibility behavior
  changes.

## Compatibility Impact

No SQL-visible behavior change is intended. Direct updates never expose handler
cursor continuation state to the SQL layer. Ordinary indexed reads keep their
existing cursor reset behavior.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format, journal, lock, sidecar, or lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No new dependency. The change is a small handler-local branch.

## Tests And Verification

Passed on 2026-05-23:

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`
- `cmake --build build/mariadb-mylite-storage-smoke --target mysqlserver
  -j1`
- `cmake --build --preset storage-smoke-dev --target
  mylite_perf_baseline mylite_embedded_storage_engine_test
  mylite_embedded_statement_test -j1`
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure` passed 2/2 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 1000 100000` reported
  prepared row-only update step at 1.527 us/op.

## Acceptance Criteria

- Direct-update exact reads do not clear cursor buffers when cursor state is
  explicitly skipped.
- Ordinary exact indexed reads still clear and publish cursor state.
- Focused embedded statement and storage-engine tests pass.
- The focused prepared row-only update benchmark does not regress.

## Risks And Unresolved Questions

- This is a small cleanup and does not address the larger prepared-update cost
  in SQL-layer DML preparation, exact-index lookup, row materialization, or
  active rewrite rollback bookkeeping.
