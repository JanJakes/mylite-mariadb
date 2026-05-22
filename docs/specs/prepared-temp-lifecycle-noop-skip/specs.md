# Prepared Temp Lifecycle No-Op Skip

## Problem

The prepared SQL policy cache records temporary-table lifecycle effects during
`mylite_prepare()`, but the successful prepared non-result execution path still
calls `apply_prepared_temporary_table_lifecycle()` for every execution. Ordinary
prepared row DML has no cached temporary-table names, so the call only checks an
empty string and empty vector before returning.

Late prepared-update sampling after storage update-path improvements still
shows the no-op helper on the hot path. The same execution path also calls
`prepared_statement_changes_temporary_table_lifecycle()` later to decide whether
storage metadata may have changed, repeating the same empty checks.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::prepare_impl()` already initializes
  `mylite_stmt::temporary_table_to_remember` and
  `temporary_tables_to_forget` from the immutable prepared SQL text.
- `packages/libmylite/src/database.cc::execute_statement()` unconditionally
  calls `apply_prepared_temporary_table_lifecycle()` after a successful
  non-result prepared execution.
- The temporary-table lifecycle predicate is derived entirely from the prepared
  SQL text and can be cached beside the prepared lifecycle names.

## Design

- Compute the cached temporary-table lifecycle predicate once during
  `mylite_prepare()`.
- Call `apply_prepared_temporary_table_lifecycle()` only when cached lifecycle
  names exist.
- Reuse the same predicate when deciding whether to call
  `note_storage_metadata_may_change()`.
- Keep all DDL, transaction-control, schema-selection, checkpoint, and warning
  behavior unchanged.

## Affected Subsystems

- `libmylite` prepared statement execution.
- MyLite temporary-table lifecycle tracking.
- Prepared update performance baseline.

## Compatibility Impact

No SQL behavior, public C API behavior, warning behavior, transaction behavior,
temporary-table lifecycle behavior, or affected-row behavior should change.
Prepared temporary-table DDL still applies its cached lifecycle state after
successful execution.

## Single-File And Embedded Lifecycle Impact

No durable file-format, sidecar, lock, journal, recovery, or open/close
lifecycle change. The slice changes only in-memory prepared statement
bookkeeping.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. The optimization runs above MariaDB storage-engine
execution after the statement has already succeeded.

## Binary-Size And Dependency Impact

Tiny first-party C++ branch change. No dependency or meaningful size-profile
impact is expected, but the storage-smoke embedded archive should be rebuilt
for the usual performance-slice size record.

## Tests And Verification Plan

- `git diff --check`
- `git clang-format --diff -- packages/libmylite/src/database.cc`
- Build `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused embedded statement and storage-engine smoke tests.
- Run the full `storage-smoke-dev` CTest preset.
- Rebuild the storage-smoke MariaDB embedded archive.
- Run `prepared-update-components` and capture a delayed steady-loop sample.

## Acceptance Criteria

- Ordinary prepared non-result statements with no cached temporary-table
  lifecycle names do not call the lifecycle apply helper on success.
- Prepared temporary-table create/drop statements still update the tracked
  temporary-table set after success.
- The metadata-epoch note still runs when cached temporary-table lifecycle names
  exist and the statement does not use an outer storage checkpoint.
- Existing embedded statement and routed storage tests pass.

## Risks And Unresolved Questions

- The skip must use the prepared-statement lifecycle flag, not a fresh SQL parse
  or repeated container checks, so the hot path remains tied to
  `mylite_prepare()` metadata.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff -- packages/libmylite/src/database.cc`: passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-storage-engine'
  --output-on-failure`: passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10 tests.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed,
  `libmariadbd.a` is 21,188,224 bytes / 20.21 MiB / 481 members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`: bind `0.023 us/op`,
  step `1.874 us/op`, reset `0.022 us/op`.
- Delayed steady-loop sample:
  `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000` with Time Profiler delay
  produced `/tmp/mylite-prepared-temp-lifecycle-flag.sample.txt`. The sample
  includes `rewrite_active_update_pages`, `find_exact_index_row_id`, and
  `Protocol::net_send_ok`, but no
  `apply_prepared_temporary_table_lifecycle` or
  `prepared_statement_changes_temporary_table_lifecycle` frames.
