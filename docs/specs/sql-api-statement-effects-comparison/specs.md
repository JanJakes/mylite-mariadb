# SQL API Statement Effects Comparison

## Goal

Extend the MariaDB embedded SQL API comparison harness to cover observable
statement effects: affected rows and generated insert ids for direct and
prepared non-result statements, including representative
`INSERT ... ON DUPLICATE KEY UPDATE` duplicate-update insert-id behavior.

MyLite already has focused direct and prepared statement-effect tests. This
slice adds baseline evidence that the public `mylite_changes()` and
`mylite_last_insert_id()` values match the raw MariaDB embedded API for a
representative temporary-table DML sequence.

## Non-Goals

- Do not import MariaDB MTR.
- Do not compare durable MyLite storage behavior against MariaDB sidecar
  storage.
- Do not claim exhaustive affected-row behavior for all SQL modes, duplicate
  modes, routed durable storage, or engine-specific edge cases.
- Do not change public API behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares `mysql_affected_rows()` and
  `mysql_insert_id()` for direct statement effects.
- `mariadb/libmariadb/include/mariadb_stmt.h` declares
  `mysql_stmt_affected_rows()` and `mysql_stmt_insert_id()` for prepared
  statement effects.
- `mariadb/sql/sql_insert.cc:2308-2321` treats duplicate-update rows like
  ordinary updates for `mysql_insert_id()` unless the query explicitly invokes
  `LAST_INSERT_ID(expr)`.
- `mariadb/sql/item_func.cc:4544-4555` implements `LAST_INSERT_ID(expr)` by
  setting the connection's client-visible insert id.
- `packages/libmylite/src/database.cc` copies direct effects from
  `mysql_affected_rows()` / `mysql_insert_id()`.
- `packages/libmylite/src/database.cc` copies prepared effects from
  `mysql_stmt_affected_rows()` / `mysql_stmt_insert_id()`.

## Compatibility Impact

The comparison harness now proves representative direct and prepared
insert/update/delete effects plus duplicate-update ODKU insert-id effects
against the MariaDB embedded baseline. This strengthens the SQL execution API
compatibility claim without claiming broad MTR-scale coverage.

## Design

Extend `mylite_embedded_comparison_test` with a temporary-table statement
effect sequence:

1. Create and select a runtime schema.
2. Create a temporary autoincrement table.
3. Run direct multi-row `INSERT`, ODKU duplicate update, ODKU
   `LAST_INSERT_ID(id)` duplicate update, ordinary `UPDATE`, and `DELETE`.
4. Run prepared `INSERT`, ODKU `LAST_INSERT_ID(id)` duplicate update, ordinary
   `UPDATE`, and `DELETE`.
5. Record affected rows and insert ids after each non-result statement.
6. Compare the raw MariaDB observations with the MyLite public API
   observations.

Temporary tables keep the comparison focused on SQL API behavior rather than
durable storage implementation. The raw MariaDB phase can use its private
runtime directory, while the MyLite phase can use the same public APIs without
creating durable table sidecars.

## Single-File And Embedded-Lifecycle Impact

No MyLite file-format change. The MyLite phase opens a normal `.mylite` file
and uses only runtime-schema plus temporary-table state for this comparison.

## Public API And File-Format Impact

No public API or file-format change.

## Build, Size, And Dependencies

No dependency or shipped-library size impact. The comparison test already links
the embedded MariaDB archive.

## Test And Verification Plan

1. Extend `mylite_embedded_comparison_test` with direct and prepared statement
   effect observations.
2. Include direct ODKU duplicate-update insert-id observations with and without
   `LAST_INSERT_ID(id)`, plus prepared ODKU `LAST_INSERT_ID(id)`.
3. Run the `sql-comparison` compatibility harness group.
4. Run embedded preset tests, format checks, tidy, shell checks, and
   `git diff --check`.

## Acceptance Criteria

- Direct insert/update/delete effect observations match raw MariaDB.
- Direct ODKU duplicate-update insert-id observations match raw MariaDB,
  including the `LAST_INSERT_ID(id)` idiom.
- Prepared insert/update/delete effect observations match raw MariaDB.
- Prepared ODKU `LAST_INSERT_ID(id)` insert-id observations match raw MariaDB.
- Compatibility docs no longer list statement-effect baseline comparison as
  planned.

## Risks And Open Questions

- This is representative coverage over temporary tables. Durable routed
  storage DML compatibility remains covered by storage-smoke tests and should
  not be inferred from this comparison alone.
