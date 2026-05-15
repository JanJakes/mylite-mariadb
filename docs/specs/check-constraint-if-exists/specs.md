# CHECK Constraint IF EXISTS

## Goal

Cover MariaDB-compatible existence options for supported table-level CHECK
constraint ALTER on MyLite-routed tables. Duplicate
`ADD CONSTRAINT IF NOT EXISTS` should warn and leave existing CHECK metadata
unchanged, missing `ADD CONSTRAINT IF NOT EXISTS` should add a new CHECK, missing
`DROP CONSTRAINT IF EXISTS` should warn without mutation, and existing
`DROP CONSTRAINT IF EXISTS` should remove the CHECK through the MyLite
copy-rebuild path.

## Non-Goals

- Do not cover foreign-key, unique, primary-key, period, generated-column, or
  field-level CHECK existence-option variants in this slice.
- Do not implement online/in-place CHECK ALTER; MyLite's supported ALTER paths
  remain copy-rebuild based.
- Do not expand CHECK expression coverage beyond simple deterministic
  predicates.
- Do not claim broader SQL rollback for CHECK ALTER failures.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB ALTER TABLE documentation lists constraint add/drop syntax and
  describes `IF EXISTS` / `IF NOT EXISTS` ALTER behavior. Source:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/alter/alter-table>
- MariaDB constraint documentation describes table-level CHECK constraints and
  `ALTER TABLE ... ADD CONSTRAINT ... CHECK (...)`. Source:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/constraint>
- `mariadb/sql/sql_yacc.yy:6171-6175` parses `constraint_def` and records CHECK
  constraints through `Lex->add_constraint()`.
- `mariadb/sql/sql_yacc.yy:8005-8013` parses `ALTER TABLE ... ADD CONSTRAINT`
  and the CHECK-specific `ADD CONSTRAINT IF NOT EXISTS name CHECK (...)` form.
- `mariadb/sql/sql_yacc.yy:8040-8049` parses
  `DROP CONSTRAINT [IF EXISTS] name` into an `Alter_drop::CHECK_CONSTRAINT`.
- `mariadb/sql/sql_lex.h:4486-4493` stores the constraint name and the
  `if_not_exists` flag on the CHECK metadata object.
- `mariadb/sql/sql_table.cc:6429-6441` checks whether a requested CHECK drop
  names an existing table-level CHECK when `IF EXISTS` is present.
- `mariadb/sql/sql_table.cc:6496-6502` emits note-level diagnostics and removes
  missing `DROP ... IF EXISTS` entries from the ALTER list.
- `mariadb/sql/sql_table.cc:6740-6767` handles duplicate
  `ADD CONSTRAINT IF NOT EXISTS`, warns, and removes duplicate CHECK entries
  from the ALTER list.
- `mariadb/sql/sql_show.cc:10741-10750` exposes CHECK metadata through
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.

## Compatibility Impact

This moves a narrow part of broader constraint DDL from planned to covered:

- duplicate named table-level CHECK additions with `IF NOT EXISTS` are no-op
  warnings;
- missing named table-level CHECK additions with `IF NOT EXISTS` publish new
  catalog metadata and enforce new rows;
- missing named table-level CHECK drops with `IF EXISTS` are no-op warnings;
- existing named table-level CHECK drops with `IF EXISTS` remove metadata and
  enforcement across close/reopen.

The behavior remains partial because foreign keys, unique constraints, broader
CHECK expressions, generated-column interactions, explicit online/in-place
algorithm variants, and broader rollback remain separate work.

## Proposed Design

No production code change is expected unless the storage-smoke test exposes a
MariaDB path that still assumes durable `.frm` sidecars. MyLite already routes
supported CHECK ALTER through copy rebuilds and stores the rebuilt MariaDB
table-definition image in the `.mylite` catalog.

The test should prove the SQL-layer existence-option semantics and MyLite
catalog publication by checking `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`,
constraint enforcement, warnings, close/reopen discovery, and sidecar gates.

## Affected Subsystems

- MariaDB parser and ALTER metadata handling, inherited unchanged.
- MyLite copy ALTER and catalog publication through the stored table-definition
  image.
- MyLite storage-smoke routed-DDL/CHECK/sidecar harness groups and compatibility
  docs.

## DDL Metadata Routing Impact

CHECK existence-option ALTER publishes or skips MariaDB table-definition
metadata. Supported successful changes must update only the MyLite catalog in
the primary `.mylite` file, without durable `.frm` files or engine sidecars.

## Single-File And Lifecycle Impact

No new companion file type is introduced. The test must prove final close leaves
no forbidden durable sidecars and that close/reopen discovers the committed
CHECK metadata state from the `.mylite` file.

## Public API And File-Format Impact

No public C API or first-party file-format change is expected. Existing warning
and error APIs expose MariaDB diagnostics for no-op skips and CHECK violations.

## Storage-Engine Routing Impact

This applies to the current supported routed table shapes. Requested engines
such as `InnoDB` still resolve to the effective MyLite handler through the
existing routing policy.

## Binary-Size, License, And Dependency Impact

No binary-size-sensitive dependency, license, or build-profile change is
expected.

## Test Plan

1. Add storage-smoke coverage for duplicate
   `ADD CONSTRAINT IF NOT EXISTS existing CHECK (...)` warnings.
2. Assert duplicate additions do not create duplicate rows in
   `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.
3. Add storage-smoke coverage for missing
   `ADD CONSTRAINT IF NOT EXISTS new_name CHECK (...)` publishing a new CHECK.
4. Assert the new CHECK rejects incompatible inserted rows.
5. Add storage-smoke coverage for missing
   `DROP CONSTRAINT IF EXISTS missing_name` warnings with no metadata mutation.
6. Add storage-smoke coverage for existing
   `DROP CONSTRAINT IF EXISTS new_name` removing the CHECK.
7. Assert the dropped CHECK no longer rejects rows, while remaining CHECKs still
   do.
8. Close/reopen and verify committed CHECK metadata and enforcement.
9. Run format, focused storage-smoke tests, compatibility harness routed-DDL,
   CHECK, and sidecar reports, clang-tidy, and the `dev`, `embedded-dev`, and
   `storage-smoke-dev` gates.

## Acceptance Criteria

- Duplicate `ADD CONSTRAINT IF NOT EXISTS rating_max CHECK (...)` succeeds with
  a warning and leaves one metadata row.
- Missing `ADD CONSTRAINT IF NOT EXISTS rating_min CHECK (...)` succeeds,
  publishes one metadata row, and rejects rows below the minimum.
- Missing `DROP CONSTRAINT IF EXISTS missing_rating` succeeds with a warning and
  preserves existing CHECK metadata and enforcement.
- Existing `DROP CONSTRAINT IF EXISTS rating_min` succeeds and the dropped CHECK
  no longer rejects rows before or after close/reopen.
- The remaining CHECK metadata survives close/reopen and still rejects invalid
  rows.
- Durable sidecar gates pass.

## Risks And Unresolved Questions

- This slice intentionally avoids foreign-key and unique constraint
  existence-options because MyLite's support status for those surfaces differs
  from table-level CHECK constraints.
- The coverage is representative rather than exhaustive; conflict matrices,
  multiple CHECKs in one statement, and field-level CHECK variants remain
  separate work.
