# REPLACE Row Rollback Coverage

## Problem

MyLite row-DML rollback coverage includes failed multi-row `INSERT`, failed
`UPDATE`, and prepared variants, but it does not directly exercise SQL
`REPLACE`. MariaDB defines `REPLACE` as insert-or-delete-plus-insert, so a
failed later row in the same statement can follow an already visible replacement
of an earlier row. MyLite must prove that the statement transaction hook restores
the original row and index state for routed tables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_parse.cc:607-614` marks `SQLCOM_REPLACE` and
  `SQLCOM_REPLACE_SELECT` as data-changing insert-style statements.
- `mariadb/sql/sql_insert.cc:2118-2134` documents `REPLACE` as either
  `INSERT` or `DELETE` plus `INSERT`.
- `mariadb/sql/sql_insert.cc:2134-2197` implements
  `Write_record::replace_row()`, first trying `ha_write_row()`, then resolving
  duplicate keys by updating or deleting the conflicting row before retrying
  the insert.
- `mariadb/sql/sql_insert.cc:2402-2424` routes duplicate mode `DUP_REPLACE`
  through `Write_record::replace_row()`.
- `docs/specs/transaction-handler-hooks/specs.md` includes `REPLACE` in the
  row-DML statements expected to flow through MariaDB's statement transaction
  hook path.

## Scope

- Direct failed multi-row `REPLACE` over a routed `ENGINE=InnoDB` table.
- Prepared failed multi-row `REPLACE` over the same table shape.
- Preservation of original rows, unique-key visibility, and close/reopen
  visibility after the failed statements.

## Non-Goals

- Full SQL transaction, savepoint, or crash-safe statement undo semantics.
- `REPLACE ... SELECT`, triggers, foreign keys, partitioned tables, or
  multi-table interactions.
- Autoincrement gap compatibility for failed `REPLACE` statements.

## Design

Extend the existing storage-engine rollback smoke table:

1. Start from a routed `ENGINE=InnoDB` table with primary and unique keys plus a
   CHECK constraint.
2. Run a direct two-row `REPLACE` where the first row replaces an existing
   primary-key row and the second row fails the CHECK constraint.
3. Assert the replacement title is not visible and the original row remains.
4. Repeat the same shape through `mylite_prepare()` and bound parameters.
5. Reopen the database and assert the failed replacement rows remain invisible.

No production code change is expected if the current statement hook path is
correct.

## Compatibility Impact

Statement rollback remains partial, but it now has direct evidence for
`REPLACE`, a row-DML path where MariaDB may delete or update an existing row
while resolving duplicate keys. This strengthens routed `ENGINE=InnoDB`
compatibility without claiming multi-statement transaction support.

## File Lifecycle

No file-format or companion-file behavior changes. The test uses the existing
primary `.mylite` file and the existing statement checkpoint lifecycle.

## Test And Verification Plan

- Extend `libmylite.embedded-storage-engine` with direct and prepared failed
  `REPLACE` rollback checks.
- Run the focused storage-smoke test.
- Run the `statement-rollback` compatibility report.
- Run formatting, shell, whitespace, and tidy checks used by adjacent rollback
  slices.

## Acceptance Criteria

- Failed direct `REPLACE` leaves the original row visible and the replacement
  row invisible.
- Failed prepared `REPLACE` leaves the original row visible and the replacement
  row invisible.
- The same visibility holds after close/reopen.
- Roadmap, compatibility, and harness docs name `REPLACE` row-DML rollback as
  covered without implying full SQL transaction support.
