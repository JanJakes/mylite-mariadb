# Scoped Header Read Fast Path

## Problem Statement

Prepared primary-key updates still show `active_statement_for_file()` samples
under `read_header()` even in paths that already opened the file through a
scoped helper. The current indexed-row lookup and row-update code know the
matching active write/read statement from `open_existing_file_scope()` or
`open_existing_file_for_update_scope()`, but then call the generic
`read_header()` helper, which rediscovers the same owner from the `FILE *`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns the affected paths in
  `packages/mylite-storage/src/storage.c`.
- `open_existing_file_scope()` records active write statements, active read
  statements, and active read snapshots in `mylite_storage_file_scope`.
- `open_existing_file_for_update_scope()` records active write and active read
  statements in `mylite_storage_update_file_scope`.
- `read_header()` already has the correct in-memory fast path, but reaches it
  by walking active statement chains from `FILE *`.
- The sampled prepared-update run after cached buffered row rewrites still
  showed `active_statement_for_file()` under `read_header()` in
  `mylite_storage_find_indexed_row_reuse()` and update execution.

## Proposed Design

- Add scoped header-read helpers for `mylite_storage_file_scope` and
  `mylite_storage_update_file_scope`.
- Return the active statement's decoded current header, active read statement
  header, active read snapshot header, or transaction-journal snapshot header
  directly when the scope already proves that ownership.
- Fall back to generic `read_header()` for standalone file handles and any
  uncommon state not represented by the scope.
- Use the scoped helpers in exact index-entry lookup, indexed-row payload
  lookup, and row-update execution.

## Affected Subsystems

- First-party MyLite storage read/update hot paths.
- Active statement/read-snapshot header lookup.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. The helpers return the same decoded header that the generic helper
would return after rediscovering ownership.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change. Statement-owned files and
snapshot files retain the same open/close behavior.

## Binary Size, License, And Dependencies

Small first-party helper split with no new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Exact index-entry lookup, indexed-row lookup, and row-update storage paths
  use scoped header reads after opening their file scope.
- Existing active statement, read statement, read snapshot, and transaction
  snapshot behavior remains unchanged.
- Storage and embedded tests pass.
- Prepared-update profiling reduces `active_statement_for_file()` samples under
  header reads or shows the next bottleneck clearly.

## Risks And Open Questions

- The scoped helpers must preserve transaction-journal snapshot behavior for
  standalone snapshot file handles, which are represented globally rather than
  directly on `mylite_storage_file_scope`.
