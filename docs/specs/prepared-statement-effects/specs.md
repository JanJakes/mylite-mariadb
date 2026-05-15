# Prepared Statement Effects

## Goal

Cover prepared-statement affected-row and generated insert-id behavior for
representative non-result statements.

The public API already exposes `mylite_changes()` and
`mylite_last_insert_id()`. Existing prepared-statement tests cover a
parameterized insert. This slice adds explicit prepared `UPDATE` and `DELETE`
coverage so the SQL execution API has the same representative statement-effect
coverage for prepared execution that direct execution already has.

## Non-Goals

- Do not change public API behavior.
- Do not broaden statement-effect coverage to multi-result statements.
- Do not claim raw MariaDB system-table or MTR-scale comparison coverage.
- Do not add transaction, savepoint, or multi-statement rollback semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c:1188-1190` implements
  `mysql_stmt_affected_rows()` by returning the prepared statement's last
  `upsert_status.affected_rows`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c:2486-2488` implements
  `mysql_stmt_insert_id()` by returning the prepared statement's last
  `upsert_status.last_insert_id`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c:2187-2190` rejects execution
  when required prepared parameters are not bound.
- `packages/libmylite/src/database.cc:1485-1499` already copies
  `mysql_stmt_insert_id()` and maps `mysql_stmt_affected_rows()` into the
  database handle after successful non-result prepared execution.

## Compatibility Impact

Prepared execution should report MariaDB-compatible statement effects through
the same public accessors as direct execution:

- multi-row prepared `INSERT` reports the affected row count and first
  generated autoincrement id;
- prepared `UPDATE` reports affected rows;
- prepared `DELETE` reports affected rows;
- a later prepared `INSERT` reports the next generated autoincrement id.

This does not claim exhaustive affected-row behavior for every SQL mode,
duplicate-key variant, or engine-specific edge case.

## Design

Extend `mylite_embedded_statement_test` with a prepared statement-effect case.
The test opens a normal embedded MyLite database, creates a runtime schema and
temporary autoincrement table, then executes prepared `INSERT`, `UPDATE`,
`DELETE`, and another `INSERT` with parameter bindings.

Temporary tables keep the test focused on public SQL API behavior. Durable
MyLite row storage has separate storage-engine coverage.

## File Lifecycle

The test creates only transient session state under the per-test runtime
directory. Existing cleanup still requires the runtime directory to be empty
after `mylite_close()`.

## Embedded Lifecycle And API

No handle ownership changes. The effects are read from the database handle
immediately after each successful `mylite_step()` returns `MYLITE_DONE` for a
non-result prepared statement.

## Build, Size, And Dependencies

No production code, dependency, or build-profile change is expected.

## Test Plan

1. Add embedded prepared-statement coverage for temporary-table `INSERT`,
   `UPDATE`, `DELETE`, and a second `INSERT`.
2. Assert `mylite_changes()` after each non-result prepared statement.
3. Assert `mylite_last_insert_id()` after generated autoincrement inserts.
4. Verify final row visibility through a prepared `SELECT`.
5. Run the prepared-statement compatibility group and the embedded preset.
6. Run format, tidy, diff, and relevant full preset checks before commit.

## Acceptance Criteria

- Prepared non-result statements have committed tests for affected row counts.
- Prepared generated autoincrement inserts have committed tests for last insert
  id.
- API, compatibility, roadmap, and harness docs describe the coverage without
  overstating broad MariaDB baseline comparison.

## Risks And Open Questions

- The test uses temporary MariaDB tables to isolate public API behavior from
  durable storage. Broader routed-storage DML behavior remains covered by the
  storage-smoke suite.
