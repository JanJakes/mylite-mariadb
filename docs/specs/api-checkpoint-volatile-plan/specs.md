# API Checkpoint Volatile Plan

## Problem

The libmylite C API wraps transactional row-DML execution in
`StorageStatementCheckpoint`. That checkpoint always creates a MEMORY/HEAP
snapshot, even when a prepared statement targets a durable table whose rollback
is already covered by the durable MyLite storage checkpoint. The prepared
primary-key update benchmark still samples `mylite_volatile_begin_snapshot()`
from this API wrapper after the handler-level durable path has been narrowed.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- libmylite owns the embedded session lifecycle. This slice records successful
  `USE <schema>` statements in the libmylite handle so conservative catalog
  lookup does not depend on MariaDB client-handle internals.
- `packages/libmylite/src/database.cc` marks prepared row-DML statements with
  `uses_transaction_statement_checkpoint`.
- `StorageStatementCheckpoint::begin()` always calls
  `mylite_volatile_begin_snapshot()` after
  `mylite_storage_begin_statement()`.
- `row_dml_target_table_name()` already recognizes simple single-table
  `INSERT`, `REPLACE`, `UPDATE`, and `DELETE` targets, and temporary table
  tracking already identifies `CREATE TEMPORARY TABLE` and `DROP TABLE`
  lifecycle changes.
- MyLite table metadata stores the requested engine name. MEMORY and HEAP are
  volatile row stores even though the effective engine is MyLite.

## Design

- Extend `StorageStatementCheckpoint::begin()` with a volatile-snapshot
  requirement flag.
- Keep DDL and unknown row-DML behavior conservative: when the API cannot prove
  the row target is durable, it still creates the volatile snapshot.
- For prepared row-DML, cache a per-statement volatile-snapshot plan. The plan
  proves durable-only by combining:
  - an unqualified single-table row-DML target,
  - the current libmylite-tracked session schema,
  - table-engine metadata cached from successful local table DDL or read from
    the MyLite storage catalog, and
  - the tracked temporary-table list.
- Recompute the prepared plan after local storage metadata or temporary table
  lifecycle changes.
- Clear the table-engine cache on DDL that is not a simple local `CREATE TABLE`
  or `DROP TABLE`, keeping hard-to-classify metadata changes conservative.
- Keep transaction and savepoint snapshots eager. This slice only removes
  redundant statement-level volatile snapshots from proven durable-only row DML.

## Compatibility Impact

No SQL, public C API, result-code, or storage-engine routing behavior changes.
Prepared row-DML statements that target MEMORY, HEAP, temporary tables,
qualified names, unknown tables, or statements whose target cannot be parsed
remain conservative and keep the statement-level volatile snapshot.

## Single-File And Lifecycle Impact

No file-format or durable companion lifecycle changes. The optimization only
changes whether process-owned volatile row snapshots are created for a
statement.

## Tests And Verification

- Add prepared C API coverage for durable and MEMORY row-DML inside an explicit
  transaction.
- Add statement-error rollback coverage for a prepared MEMORY insert that
  violates a unique key, proving volatile statement snapshots are still created
  when needed.
- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Prepared durable-only row-DML no longer samples API-level
  `mylite_volatile_begin_snapshot()` in the hot prepared-update phase.
- Prepared MEMORY/HEAP row-DML still rolls back at statement and transaction
  boundaries.
- Unknown or hard-to-prove row-DML stays conservative.
- Storage-smoke tests pass.

## Risks And Unresolved Questions

- The SQL classifier is intentionally simple and only proves durable-only for
  unqualified single-table row DML. Broader SQL forms can be optimized later
  after parser-backed target discovery exists.
