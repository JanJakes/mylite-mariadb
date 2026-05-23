# MTR routed storage DML effects smoke

## Problem

The storage-routed MTR runner proves DDL routing and selected row visibility,
but it does not yet exercise common mutating DML statement effects through the
raw embedded MTR path. MyLite already has first-party coverage for routed
statement effects and row/index mutation, but MTR storage mode should also
prove representative `ENGINE=InnoDB` duplicate-update, replacement, update,
delete, affected-row, and insert-id behavior against the static MyLite handler.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_insert.cc::mysql_insert()` prepares and executes ordinary
  `INSERT`, `REPLACE`, and `INSERT ... ON DUPLICATE KEY UPDATE` statements.
- `mariadb/sql/sql_insert.cc::Write_record::write_record()` handles duplicate
  modes and dispatches accepted writes, duplicate updates, and replacements
  through handler row operations.
- `mariadb/sql/sql_update.cc::mysql_update()` dispatches single-table updates
  through `ha_update_row()` or handler direct-update paths when available.
- `mariadb/sql/sql_delete.cc::mysql_delete()` dispatches deletes through
  `ha_delete_row()`.
- `mariadb/storage/mylite/ha_mylite.cc::write_row()`,
  `ha_mylite::update_row()`, and `ha_mylite::delete_row()` publish accepted
  row/index mutations into MyLite storage.
- MariaDB documents `INSERT ... ON DUPLICATE KEY UPDATE`, `REPLACE`,
  `ROW_COUNT()`, and `LAST_INSERT_ID()` statement semantics:
  <https://mariadb.com/kb/en/insert-on-duplicate-key-update/>,
  <https://mariadb.com/kb/en/replace/>,
  <https://mariadb.com/kb/en/row_count/>, and
  <https://mariadb.com/kb/en/last_insert_id/>.

## Design

Add `mylite.routed_storage_dml_effects` to the storage MTR list. The test runs
with a primary `.mylite` file, enforces MyLite storage, creates an explicit
`ENGINE=InnoDB` table, and verifies:

- auto-increment insert statement effects;
- `ON DUPLICATE KEY UPDATE` over a primary-key conflict, including
  `LAST_INSERT_ID(id)` publication and updated row contents;
- inserting a new row through the same ODKU statement shape;
- `REPLACE` over an existing row;
- keyed `UPDATE` and `DELETE` affected-row counts; and
- final row/index visibility without native durable sidecars.

## Scope

This is test and documentation work only. It does not change DML execution,
autoincrement allocation, statement-effect APIs, storage indexes, public APIs,
or file format.

## Compatibility Impact

The storage MTR runner gains raw embedded evidence for representative routed
MyLite DML statement effects on a table requested as `ENGINE=InnoDB`. Broader
ODKU, `REPLACE`, trigger, view, grouped autoincrement, and exhaustive
affected-row matrices remain covered by first-party tests or planned separately.

## Storage And Lifecycle Impact

All durable row, autoincrement, and index state remains in the primary
`.mylite` file. The test reuses the sidecar assertion to reject native schema
directories and native engine sidecars for the MyLite-owned schema.

## Verification Plan

- `tools/mylite-mtr-harness run-storage mylite.routed_storage_dml_effects`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new storage MTR DML effects test passes.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention routed DML effects MTR coverage.

## Risks

The selected statement-effect checks are representative. They do not replace
the broader direct/prepared MyLite API statement-effect suites.
