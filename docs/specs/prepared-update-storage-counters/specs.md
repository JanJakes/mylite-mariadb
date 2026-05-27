# Prepared Update Storage Counters

## Problem

The current VPS storage-smoke run shows prepared primary-key update component
steps around `249-315 us/op`, while the prepared-insert branch and tail scan
counters are already near zero. Existing update specs describe earlier SQL and
handler fast paths, but the benchmark output does not show whether current
prepared update time is still storage wrapper work, active-statement row reads,
or index-preserving update writes.

Without focused counters, the next update-performance slice would have to guess
between SQL planning, handler direct-update dispatch, and first-party storage
mutation work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::execute_inner()` can execute a
  prepared MyLite direct-update shape through
  `execute_mylite_prepared_direct_update()`, or fall back to
  `update_single_table()`.
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  reads the target primary-key row through
  `mylite_storage_find_indexed_row_in_statement_into()` when an active storage
  checkpoint exists.
- `mariadb/storage/mylite/ha_mylite.cc::direct_update_row_preserving_index_entries()`
  and `ha_mylite::update_row()` both use
  `mylite_storage_update_row_preserving_index_entries_in_statement()` for
  active row-only updates that do not change index entries.
- `packages/mylite-storage/src/storage.c` already exposes storage test-hook
  counters for prepared-insert branch planning, tail scans, and scan
  breakdowns under `MYLITE_STORAGE_TEST_HOOKS`; the performance harness prints
  those counters only in focused phases.

## Design

- Add storage test-hook counters for:
  - indexed-row reads through filename scope,
  - indexed-row reads through an active statement,
  - preserving-index updates through filename scope,
  - preserving-index updates through an active statement,
  - changed-index updates through filename scope,
  - changed-index updates through an active statement.
- Reset and print the counters around prepared update component loops in
  `tools/mylite_perf_baseline.c`.
- Keep the counters in first-party storage code so the storage-smoke benchmark
  can be rebuilt without changing the MariaDB embedded archive.
- Add unit coverage proving the counters distinguish filename-scope and
  statement-scope indexed-row reads and preserving-index updates.

## Affected Subsystems

- MyLite storage test hooks.
- Storage-smoke performance baseline output.
- Storage unit tests.

## Compatibility Impact

No SQL behavior, handler behavior, diagnostics, warning, affected-row,
storage-engine routing, or public API behavior changes. The counters are
test-hook-only observability.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change. The counters are thread-local process memory and do not affect
checkpoint publication.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. The benchmark continues to route `ENGINE=InnoDB`
through MyLite storage.

## Binary-Size And Dependency Impact

The production storage library already omits `MYLITE_STORAGE_TEST_HOOKS`; the
storage-smoke/test builds gain a few thread-local counters and accessors. No new
dependency or license impact.

## Test And Verification Plan

- Add storage unit coverage for the new counter reset/accessor behavior.
- Run formatting and diff checks.
- Run the dev storage unit target and CTest storage gate.
- Run storage-smoke storage and embedded storage-engine tests.
- Run prepared update and prepared insert component benchmarks to record local
  counter evidence and confirm insert counters remain unchanged.

## Acceptance Criteria

- Prepared update component benchmark output includes storage read/update shape
  counters.
- Statement-scope reads and preserving-index updates are counted separately
  from filename-scope calls.
- Existing storage and storage-smoke tests pass.
- The slice does not change SQL-visible behavior or storage file format.

Verification results:

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed with no formatting diff.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  `1/1` in `298.07s`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed `2/2` in `313.27s`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  reported prepared primary-key update step `249.172 us/op`, with
  `100000` statement-scope indexed-row reads and `100000` statement-scope
  changed-index writes.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 1000 100000`
  reported prepared row-only update step `263.183 us/op`, with `100000`
  statement-scope indexed-row reads and `100000` statement-scope
  preserving-index writes.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  kept the prepared-insert branch/tail storage counters at zero except the
  existing `2` packed-tail missing-page blockers.

## Risks And Unresolved Questions

- These storage counters do not directly prove whether MariaDB used the
  prepared direct-update executor, because both the ordinary and direct update
  paths can call the same active storage wrappers. A later handler or SQL-layer
  counter may be useful if storage counters show little work.
