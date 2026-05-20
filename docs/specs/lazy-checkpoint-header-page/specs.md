# Lazy Checkpoint Header Page

## Problem

Prepared row-DML runs each statement inside a MyLite storage checkpoint. When a
statement is nested under an active transaction, the storage layer clones the
parent checkpoint snapshot and eagerly rebuilds `statement->header_page` with
`encode_header_page()`.

The prepared-update profile shows this as repeated
`clone_parent_checkpoint_snapshot()` -> `encode_header_page()` ->
`checksum_page_zero_tail()` work at statement begin. For successful nested
statements, the encoded rollback header page is not read or written; it is only
needed if the nested statement rolls back.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MyLite-owned storage source:
  `packages/mylite-storage/src/storage.c`.
- `begin_checkpoint()` creates a `mylite_storage_statement` for each storage
  statement.
- `initialize_checkpoint_statement()` calls
  `clone_parent_checkpoint_snapshot()` for nested checkpoints.
- `clone_parent_checkpoint_snapshot()` currently calls `encode_header_page()`
  even though nested-statement commit only propagates the decoded
  `current_header` to the parent.
- `mylite_storage_rollback_statement()` is the path that needs the original
  `statement->header_page` bytes to restore page `0`.

## Design

Track whether a statement's rollback header page bytes are materialized:

- Add a private `has_header_page` flag to `mylite_storage_statement`.
- Set the flag when the header page is copied from disk, a read checkpoint
  cache, or a transaction-journal snapshot.
- For nested checkpoint clones, copy decoded header state and catalog page state
  from the parent, but leave `header_page` unmaterialized.
- Before rollback writes page `0`, materialize `statement->header_page` from the
  decoded rollback `statement->header`.
- Keep read-statement header pages materialized because read snapshots expose
  page `0` through `read_page_at()`.

This keeps the rollback behavior intact while removing eager header-page
encoding from the successful nested-statement path.

## Compatibility Impact

No SQL-visible behavior change. The optimization changes only when MyLite
rebuilds private checkpoint rollback bytes.

## File And API Impact

No public API, storage file-format, or companion-file change.

## Storage Routing Impact

No engine-routing change. The slice affects MyLite storage checkpoint internals
used by routed row-DML.

## Binary-Size Impact

Negligible private flag and helper code.

## Test And Verification Plan

- Build first-party storage targets.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample the benchmark and confirm nested checkpoint begin no longer spends hot
  samples in `encode_header_page()`.

## Acceptance Criteria

- Nested statement commit still propagates decoded current headers to the
  parent transaction.
- Nested statement rollback still restores page `0` and catalog page state.
- Storage-smoke transaction, statement-rollback, FK, generated-column, and
  application-schema coverage remains green.
- Prepared-update performance improves or the profile proves the eager header
  encode is gone without a regression.

## Verification

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_embedded_storage_engine_test` passed.
- `git diff --check` passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed:
  10/10 tests.
- Three `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` runs reported prepared primary-key
  updates at 4.019, 4.109, and 4.197 us/op.
- A sampled run no longer showed `encode_header_page()` or
  `checksum_page_zero_tail()` under nested
  `StorageStatementCheckpoint::begin()`; `clone_parent_checkpoint_snapshot()`
  remained present for struct and catalog-page copies.

## Risks

- A rollback path that writes an unmaterialized `header_page` would corrupt page
  `0`. Materialization must happen directly before rollback writes the saved
  header page.
