# Active-Write Exact-Read Scope Elision

## Problem

Prepared point updates call the MyLite handler exact unique index read path
before row replacement. That exact read currently enters
`Mylite_read_statement_scope`, which calls
`mylite_storage_begin_read_statement()` even when the THD already owns an
active MyLite write statement or transaction checkpoint.

`mylite_storage_begin_read_statement()` correctly detects the active write
statement and returns without opening a read statement, but the detection still
walks active storage state in a hot loop.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::index_read_map()` calls
  `ha_mylite::read_exact_unique_index_row_into()` for supported full-key exact
  unique reads.
- `read_exact_unique_index_row_into()` creates `Mylite_read_statement_scope`
  unconditionally for durable exact reads before calling
  `mylite_storage_find_indexed_row_into()`.
- `packages/mylite-storage/src/storage.c:mylite_storage_begin_read_statement()`
  returns without creating a read statement when `active_statement_for()`
  finds an active write checkpoint for the same file.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_begin_statement_checkpoint()`
  and `mylite_begin_transaction_checkpoint()` store active storage checkpoint
  handles in `Mylite_trx_context`.
- A one-second macOS `sample` run of prepared updates after the table-name
  identity slice still showed small residual frames in
  `mylite_storage_begin_read_statement()` and active statement checks.

## Design

- Add a small handler helper that inspects the current `Mylite_trx_context`
  and reports whether the THD already owns a storage statement or transaction
  checkpoint, falling back to storage's active-statement check for checkpoint
  ownership created outside the handler context.
- In `read_exact_unique_index_row_into()`, create `Mylite_read_statement_scope`
  only when that helper reports no active storage write checkpoint.
- Keep the existing read-statement scope for durable exact reads outside active
  handler write statements, preserving standalone read behavior and snapshot
  handling.
- Do not change storage's own defensive `mylite_storage_begin_read_statement()`
  behavior; direct storage callers still get the existing active-statement
  guard.

## Scope

In scope:

- Handler exact unique index reads used by prepared point updates.
- Local prepared-update performance evidence.

Out of scope:

- Generic cursor construction reads.
- Direct storage API changes.
- File format, catalog, transaction, recovery, or SQL semantics changes.

## Compatibility Impact

No SQL, public `libmylite` API, storage-engine routing, or result-set behavior
changes. Exact reads inside active handler row-DML statements use the same
active write checkpoint that storage would have selected after the redundant
read-scope call.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The slice only avoids redundant transient read-statement scope setup when a
write checkpoint is already active.

## Binary-Size And Dependency Impact

One small first-party handler helper. No new dependencies.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run the storage unit test, focused routed-storage CTest coverage, and the
  full `storage-smoke-dev` preset.
- Run `git diff --check` and `git clang-format --diff` on touched C++ docs.
- Run the prepared-update performance baseline and sample it to confirm the
  `mylite_storage_begin_read_statement()` frame disappears or remains
  residual outside the prepared-update hot path.

## Acceptance Criteria

- Exact unique reads inside active handler storage statements do not call
  `mylite_storage_begin_read_statement()`.
- Exact unique reads outside active handler storage statements still enter a
  read-statement scope.
- Existing routed storage tests pass.

## Risks

- The helper should prefer the handler's THD context but still defer to
  storage's active-statement check for cross-layer checkpoints that are active
  under the current storage context owner.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`: no
  changes.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed,
  rebuilding `storage/mylite/ha_mylite.cc` and
  `packages/mylite-storage/src/storage.c`; upstream override warnings were
  emitted from existing MariaDB headers.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed after the
  MariaDB archive rebuild and relinked the embedded storage-engine smoke and
  performance baseline binaries.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  2/2 passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates
  measured `2.300 us/op`; the sampled run measured `2.319 us/op`.
- A one-second macOS `sample` run of the rebuilt binary showed no
  `mylite_storage_begin_read_statement()` samples in the prepared-update hot
  path; remaining read-scope constructor samples were the disabled-scope fast
  path plus `mylite_storage_statement_active()` checks.
