# Prepared SQL Policy Cache

## Problem

The prepared update benchmark repeatedly executes one immutable SQL statement,
but the successful non-result path still reparses the prepared SQL text after
every execution for MyLite-owned policy bookkeeping:

- temporary-table lifecycle tracking,
- transaction-control state updates,
- no-op scans for ordinary DML that cannot change either state.

The 2026-05-20 `sample` profile for
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
1000 1000000` showed visible samples in `skip_sql_leading_noise()`,
`apply_successful_transaction_control()`, and
`apply_temporary_table_lifecycle()` below the prepared update loop. Those scans
are avoidable for ordinary prepared DML because the SQL text is fixed at
`mylite_prepare()` time.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::prepare_impl()` already classifies
  prepared statements for transaction control, storage writes, storage
  checkpoints, and catalog sync before creating the MariaDB `MYSQL_STMT`.
- `packages/libmylite/src/database.cc::execute_statement()` still calls
  `apply_temporary_table_lifecycle()` and
  `apply_successful_transaction_control()` for every successful non-result
  prepared statement.
- `apply_successful_transaction_control()` must still run for prepared
  transaction-control statements because it updates MyLite transaction,
  autocommit, completion-type, and read-only state after MariaDB accepts the
  statement.
- Temporary-table lifecycle state depends only on the prepared SQL text after a
  successful prepared DDL execution; it can be classified once and applied from
  cached names.

## Design

- Extend `mylite_stmt` with prepared-only temporary-table lifecycle metadata:
  one table name to remember for successful `CREATE TEMPORARY TABLE`, and a
  list of table names to forget for successful `DROP TABLE` forms.
- Populate that metadata during `prepare_impl()` using the same first-party SQL
  token helpers that direct execution already uses.
- Replace the prepared execution-time lifecycle parser with a cached apply
  helper that only mutates the tracked temporary-table list when cached names
  exist.
- Skip the successful transaction-control apply step for prepared statements
  whose cached `statement_transaction_control` is `None`.
- Keep the existing resolved-control path for parameterized transaction-control
  statements.
- Apply the same no-op transaction-control skip to direct execution after a
  successful statement when `direct_transaction_control_kind()` already proved
  that the statement is not transaction control.

## Affected Subsystems

- `libmylite` prepared statement execution.
- MyLite temporary-table tracking used by read-only transaction write checks.
- MyLite transaction-control state tracking.
- Local storage-smoke performance baseline.

## Compatibility Impact

Supported SQL behavior is unchanged. Prepared transaction-control statements
still update MyLite transaction state after successful MariaDB execution, and
prepared temporary-table DDL still updates MyLite's tracked temporary-table
set. Ordinary prepared DML no longer pays post-execution scans for policies it
cannot affect.

## Single-File And Lifecycle Impact

No file-format, durable sidecar, journal, recovery, lock, or open/close
lifecycle change. The slice only changes in-memory prepared-statement metadata
and post-execution bookkeeping.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing policy change. The optimization runs in `libmylite` around prepared
statement execution and leaves MariaDB statement planning and MyLite handler
calls unchanged.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to a small first-party C++
helper and two prepared-statement metadata fields.

## Test And Verification Plan

- Build the storage-smoke preset.
- Run `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  for prepared reset, transaction-control, unsupported-surface, and metadata
  coverage.
- Run `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  for prepared temporary-table lifecycle, read-only transaction, and routed row
  DML coverage.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` and compare local timing against the
  current branch baseline.
- Run a focused macOS `sample` pass and confirm the ordinary prepared update
  loop no longer shows execution-time `apply_temporary_table_lifecycle()` or
  no-op `apply_successful_transaction_control()` scans.
- Run `git diff --check`.

## Acceptance Criteria

- Ordinary prepared DML skips post-execution SQL rescans for transaction and
  temporary-table policies.
- Prepared transaction-control statements keep their existing state semantics.
- Prepared temporary-table create/drop statements keep their tracked lifecycle
  semantics.
- Storage-smoke tests pass.
- Performance evidence shows the removed scans in the prepared update profile.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: 10/10 tests
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `3.998 us/op` in the best focused run after the change.
- A focused three-second macOS `sample` run no longer showed
  `apply_temporary_table_lifecycle()` or ordinary prepared-update
  `apply_successful_transaction_control()` scans on the hot prepared update
  path.
- `git clang-format --diff`
- `git diff --check`

## Risks And Open Questions

- This removes wrapper overhead only. The profile still contains substantial
  MariaDB planner/executor cost, so larger performance gains require deeper
  prepared-path, handler, index navigation, and pager work.
