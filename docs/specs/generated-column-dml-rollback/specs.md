# Generated Column DML Rollback

## Goal

Broaden SQL rollback coverage for routed generated-column writes. A failed
multi-row statement that first changes generated values or generated-index
entries must restore the statement-start row and index visibility.

## Non-Goals

- Do not add new generated-column expression support.
- Do not implement triggers, views, partitioned tables, or expression indexes.
- Do not change public `libmylite` APIs, the file format, or transaction
  isolation semantics.
- Do not claim exhaustive generated-column rollback coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc` evaluates row values, calls
  `TABLE_LIST::view_check_option()`, then writes rows through
  `Write_record::write_record()`.
- `mariadb/sql/sql_update.cc` processes matching rows in order, calls
  `TABLE_LIST::view_check_option()`, then publishes changed rows through
  `handler::ha_update_row()`.
- `mariadb/sql/table.cc:TABLE::update_virtual_fields()` computes virtual and
  stored generated values for write and read paths.
- `mariadb/sql/table.cc:TABLE::verify_constraints()` reports CHECK failures
  before handler publication for the failing row.
- `mariadb/sql/handler.cc:handler::ha_write_row()` and
  `handler::ha_update_row()` route the final row images to the storage engine.
- Existing `docs/specs/statement-rollback-checkpoints/specs.md` documents the
  MyLite statement checkpoint that restores row, row-state, index-entry,
  autoincrement, and catalog visibility after covered statement failures.
- MariaDB documents generated-column syntax, indexed generated columns, and
  generated-value calculation:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/generated-columns>.

## Compatibility Impact

Generated columns remain partial, but MyLite gains direct evidence that
generated-column DML participates in the same statement rollback boundary as
ordinary row and index DML. The compatibility matrix should say generated
column coverage now includes representative failed multi-row insert and update
rollback over generated indexes, while exhaustive expression, trigger, view,
and partition matrices remain planned.

## Design

No production change is expected. Add storage-engine smoke coverage over a
routed `ENGINE=InnoDB` table with:

- a stored generated slug,
- a unique generated-column index,
- ordinary base rows that make statement-start visibility easy to assert.

The test covers two failure shapes:

1. A direct multi-row insert writes a first row with a new generated slug, then
   fails a later row through the generated unique index.
2. A prepared ordered multi-row update changes the first row's base column and
   generated slug, then fails a later row through the same generated unique
   index.

Both cases assert that generated values, base row values, and forced generated
index reads match the statement-start state before and after close/reopen.

## File Lifecycle

No durable companion files are introduced. Generated metadata stays in the
MariaDB table-definition image inside the MyLite catalog. Stored generated
values and generated-index entries remain ordinary primary-file row and index
pages protected by the existing statement checkpoint lifecycle.

## Embedded Lifecycle And API

No `libmylite` API changes are required. The behavior is observable through
direct execution, prepared execution, diagnostics, and close/reopen lifecycle
checks.

## Build, Size, And Dependencies

No dependency, license, or size-profile change is intended.

## Test Plan

- Add `mylite_embedded_storage_engine_test` coverage for generated-column DML
  rollback.
- Run the focused storage-smoke executable.
- Run the generated-column and statement-rollback harness groups.
- Run shell syntax checks and `git diff --check`.

## Acceptance Criteria

- A failed direct multi-row insert leaves no visible first-row insert and no
  generated-index entry for it.
- A failed prepared ordered multi-row update restores the first updated row's
  base column, generated value, and generated-index entry.
- The failing row's attempted generated value is not visible.
- The same visibility holds after close/reopen.
- Roadmap, compatibility, and harness docs describe the added representative
  rollback coverage without claiming exhaustive generated-column semantics.

## Risks And Open Questions

- Virtual generated columns, generated-column expression errors, triggers,
  views, and partitioned tables can take different SQL paths and remain
  separate compatibility work.
- This proves visible rollback through statement checkpoints, not physical page
  reclamation or crash-safe logical undo for a process interrupted during
  rollback.
