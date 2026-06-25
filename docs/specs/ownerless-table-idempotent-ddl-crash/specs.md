# Ownerless Table Idempotent DDL Crash

## Problem

Ownerless table-idempotent DDL coverage verifies peer refresh for
`CREATE TABLE IF NOT EXISTS` and `DROP TABLE IF EXISTS`, including duplicate
create and missing-drop no-op behavior. The remaining bounded crash boundary is
a writer killed after MariaDB completes those native no-op paths but before
MyLite publishes ownerless dictionary finish.

This slice adds hook-build recovery evidence for duplicate idempotent table
create and missing idempotent table drop.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `CREATE TABLE` with
  `opt_if_not_exists` and `DROP TABLE` with `opt_if_exists`.
- `mariadb/sql/sql_table.cc` routes an existing table with
  `IF NOT EXISTS` to the warning/no-op path, while plain duplicate create
  returns errno 1050.
- `mariadb/sql/sql_table.cc` treats missing tables under `DROP TABLE IF EXISTS`
  as successful no-op drops with diagnostics rather than failed drops.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `CREATE` and `DROP` as
  ownerless dictionary DDL and exposes the unsafe `dictionary-before-finish`
  hook after native SQL execution but before ownerless dictionary finish.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-table-idempotent-create-crash` creates an InnoDB table, inserts a
  row, then kills a writer after duplicate
  `CREATE TABLE IF NOT EXISTS` with a different `note` column definition
  reaches `dictionary-before-finish`.
- `dictionary-table-idempotent-drop-crash` creates a separate InnoDB table,
  inserts a row, then kills a writer after
  `DROP TABLE IF EXISTS` for a missing table name reaches
  `dictionary-before-finish`.

Both selectors keep a live ownerless peer open while the writer is killed. The
duplicate-create selector is promoted by
`ownerless-table-if-not-exists-live-recovery` to recover while the peer remains
live with the native file-operation marker clear after pre-execution metadata
proves the target table already exists. The missing-drop selector is promoted
by `ownerless-table-if-exists-drop-live-recovery` to recover while the peer
remains live with the native file-operation marker clear, then both selectors
verify ownerless and ordinary native reopen before and after forced `.shm`
rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate
  `CREATE TABLE IF NOT EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for missing
  `DROP TABLE IF EXISTS` no-op behavior.
- Native `.frm` and `.ibd` preservation for the real table.
- Absence of missing-table native files and metadata after no-op drop recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Temporary table idempotency.
- Partitioned table idempotency.
- `CREATE OR REPLACE TABLE`, `CREATE TABLE ... LIKE`, and CTAS crash recovery,
  which are covered by separate specs.
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent table DDL by proving a dead writer in MyLite's
dictionary publication window does not turn native no-op statements into
replacement, removal, or stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise native InnoDB `.frm` and
`.ibd` files under `datadir/app/`, ownerless live-peer metadata-only recovery
for proven no-op create/drop paths, forced `.shm` rebuild, and ordinary native
exclusive reopen.

## Native Storage Impact

The real tables are InnoDB. MyLite does not reinterpret native metadata; it
coordinates the ownerless dictionary boundary and verifies durable reopen
behavior for the preserved native tables.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-drop-crash`
- Run normal `table-idempotent-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shard, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- Duplicate idempotent create recovers while a peer remains live.
- Missing idempotent drop recovers while a peer remains live.
- Duplicate idempotent create recovery keeps the original table definition,
  leaves the attempted `note` column absent, and keeps plain duplicate create
  returning errno 1050.
- Missing idempotent drop recovery keeps the real table present and the missing
  table absent.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic no-op table DDL crash recovery, not every table
  lifecycle spelling.
- Broader randomized DDL oracle execution remains planned.
