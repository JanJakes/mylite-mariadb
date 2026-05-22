# Prepared Read-Scope Reuse

## Problem

Repeated routed prepared point reads still pay the storage read-statement
startup cost on every execution. Local storage-smoke samples on this branch show
storage primary-key row lookups at about 4 us/op, held-read row lookups at about
0.2 us/op, and routed prepared primary-key point selects at about 7.4 us/op.
The largest bounded cost before deeper index-page navigation is therefore the
open/recovery/shared-lock/header setup around short-lived read scopes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc` executes prepared statements through
  `mysql_stmt_execute_common()` and fetches rows through the prepared result
  path. MyLite's C API wraps the embedded client statement APIs in
  `packages/libmylite/src/database.cc`.
- `packages/libmylite/src/database.cc` classifies ordinary result-producing
  prepared statements as `uses_simple_result_execution` when they do not need
  transaction control, schema lifecycle, storage checkpoints, or temporary-table
  lifecycle handling.
- `mariadb/storage/mylite/ha_mylite.cc` uses
  `Mylite_read_statement_scope` around handler cursor and catalog reads. The
  handler deliberately keeps these scopes narrow because broad handler-level
  read locks can conflict with MariaDB flows that interleave reads and writes
  through separate handlers.
- `packages/mylite-storage/src/storage.c` implements
  `mylite_storage_begin_read_statement()` / `mylite_storage_end_read_statement()`.
  A read statement recovers pending journals, opens or reuses the primary file,
  takes a shared lock, and reads or borrows a checkpoint snapshot. Nested reads
  over the same file can clone the parent statement state instead of repeating
  the full setup.

## Design

Add one reusable prepared-read scope per `mylite_db` connection:

- A simple result-producing prepared statement may open the connection's
  reusable read scope before `mysql_stmt_execute()`.
- Repeated executions of the same prepared read statement reuse the retained
  scope after a fully drained result.
- Handler-local `Mylite_read_statement_scope` instances continue to stay
  narrow. When the retained outer scope is active, their nested storage read
  statements clone the already validated file/header/catalog state instead of
  repeating recovery, open, shared-lock, and checkpoint setup.
- Any direct write, prepared write, DDL checkpoint, transaction control, or
  savepoint control closes an idle retained read scope first.
- A write attempted while a different prepared read result is still active
  returns an explicit MyLite error instead of upgrading the active read snapshot
  underneath an unfinished result.
- Reset-before-drain and statement finalization close the retained read scope.

This keeps the optimization at the `libmylite` prepared statement boundary,
where MyLite can reason about public API sequencing. It does not make the
handler itself hold broad read locks.

## Non-Goals

- This slice does not implement WAL or MVCC read snapshots.
- This slice does not change direct SQL execution.
- This slice does not claim cross-process writers are never blocked by a
  long-lived read result. That remains future concurrency work.
- This slice does not replace the storage read-statement startup path, recovery
  probes, or shared-lock acquisition for uncached reads.

## Compatibility Impact

SQLite-style prepared read reuse can hold a read snapshot between repeated
executions of a prepared read statement. MyLite closes that snapshot before
connection-local writes when it is idle. If application code leaves a prepared
read result active and attempts a write through another statement on the same
connection, MyLite reports an explicit unsupported active-read/write
interleaving instead of allowing a potentially inconsistent handler flow.

## Single-File And Lifecycle Impact

The retained scope owns the same primary `.mylite` file handle and shared lock
as an ordinary storage read statement. No new durable files are introduced.
Existing MyLite-owned journal and transaction-journal recovery rules remain
unchanged.

## Public API And File-Format Impact

No public C API, storage API, or file-format changes are required.

## Storage-Engine Routing Impact

The optimization applies to routed MyLite handler reads. Volatile
`MEMORY`/`HEAP` and `BLACKHOLE` behavior remains unchanged because those paths
already bypass durable read scopes where appropriate.

## Binary-Size Impact

Expected binary-size impact is small: a few `libmylite` fields and helpers.

## Test And Verification Plan

- Add embedded `libmylite` coverage that repeated prepared reads remain correct
  across reset/re-execute and that a direct write after a drained prepared read
  invalidates the retained scope and is visible to the next read.
- Add coverage that write attempts while a prepared read result is still active
  fail explicitly.
- Run storage unit tests, embedded statement tests, compatibility storage-engine
  tests, and targeted performance phases:
  `prepared-pk-selects`, `prepared-pk-select-reset-after-row`,
  `storage-read-statements`, and `storage-pk-row-lookups-one-read`.

## Acceptance Criteria

- Repeated prepared primary-key point selects avoid per-execution full storage
  read-statement setup once the prepared read scope is hot.
- Direct and prepared writes close idle retained read scopes before executing.
- Active read/write interleaving is explicit and tested.
- Existing routed storage, sidecar, transaction, and prepared statement tests
  continue to pass.

## Verification

Implemented in this slice and verified locally with:

- `ctest --test-dir build/storage-smoke-dev -R 'libmylite\.(embedded-statement|embedded-storage-engine|embedded-exec|embedded-handler-savepoint)' --output-on-failure`
- `tools/mylite-compat-harness run public-api prepared-statement storage-engine locking transaction`
- `tools/mylite-perf-baseline --phase=storage-read-statements 1000 10000`
- `tools/mylite-perf-baseline --phase=storage-pk-row-lookups-one-read 1000 10000`
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`
- `tools/mylite-perf-baseline --phase=prepared-pk-select-reset-after-row 1000 10000`
- `git diff --check`

The local storage-smoke sample measured read-statement begin/end at
3.955 us/op, held-read primary-key row lookups at 0.225 us/op, fully drained
routed prepared primary-key point selects at 2.805 us/op, and reset-after-row
prepared primary-key point selects at 7.692 us/op. The reset-after-row path
continues to close the retained scope because the result is abandoned before
the storage snapshot is fully drained.

## Risks And Unresolved Questions

- Retained read scopes can block cross-process writers until the statement is
  reset, finalized, or invalidated by a same-connection write. WAL/MVCC work is
  the correct long-term fix.
- Broadening the retained scope beyond simple result-producing prepared
  statements would need a separate design because transaction control, DDL, and
  row-DML statements have checkpoint and lifecycle requirements.
