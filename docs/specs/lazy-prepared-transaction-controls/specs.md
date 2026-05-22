# Lazy Prepared Transaction Controls

## Problem

Prepared statement execution constructs an empty
`std::vector<TransactionControlKind>` on every call to `execute_statement()`.
That vector is only needed for prepared parameterized transaction-control
statements such as `SET autocommit = ?`, but it is still constructed and
destroyed for ordinary prepared row-DML.

Local prepared-update samples show the empty vector destructor in the hot
prepared `UPDATE` step path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The change is first-party `libmylite` runtime work in
  `packages/libmylite/src/database.cc`; no upstream MariaDB source change is
  required.
- `prepare_impl()` classifies transaction-control statements once and stores
  `statement.statement_transaction_control`.
- Only `TransactionControlKind::SetParameterizedTransactionControl` needs
  `resolve_parameterized_transaction_controls()` and the resolved vector.
- Ordinary prepared row-DML has
  `statement.statement_transaction_control == TransactionControlKind::None`,
  so the old unconditional local vector stayed empty and was only destroyed.

## Design

- Replace the unconditional vector local in `execute_statement()` with a lazy
  optional vector.
- Emplace and resolve the vector only when the statement control kind is
  `SetParameterizedTransactionControl`.
- Apply resolved controls through the optional vector only on that same
  transaction-control path.
- Leave all direct, prepared, savepoint, autocommit, completion-type, and
  transaction-characteristic behavior unchanged.

## Affected Subsystems

- `libmylite` prepared statement execution.
- Prepared transaction-control SQL handling.
- Prepared row-DML hot path.

## Compatibility Impact

No SQL semantics, diagnostics, public API, storage-engine routing, metadata,
transaction, or file-format behavior changes. Parameterized transaction-control
statements still resolve before execution and apply only after successful
execution.

## Single-File And Embedded Lifecycle Impact

No durable file, sidecar, journal, lock, recovery, or lifecycle change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Uses C++17 `std::optional`, already within the project C++ standard. No new
external dependency.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- packages/libmylite/src/database.cc`
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-storage-engine'
  --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000`
- Focused macOS sample of the same prepared-update component phase.

Completed verification:

- `git diff --check` passed.
- `git clang-format --diff -- packages/libmylite/src/database.cc` passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-storage-engine'
  --output-on-failure` passed 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000` measured the prepared
  update step at 1.631 us/op on the first run and 1.691 us/op during the
  sampled rerun.
- The focused sample in
  `/tmp/mylite-lazy-prepared-transaction-controls.sample.txt` no longer shows
  the resolved `std::vector<TransactionControlKind>` destructor on the hot
  path. One optional guard destructor sample remains.

## Acceptance Criteria

- Ordinary prepared row-DML no longer constructs the resolved transaction
  controls vector.
- Parameterized transaction-control prepared statements keep existing behavior.
- Existing embedded statement and storage-engine tests pass.
- Benchmark/profile notes record the resulting hot-path shape.

## Risks And Unresolved Questions

- The optional must be emplaced before resolving controls and must still be
  present when applying successful resolved controls.
