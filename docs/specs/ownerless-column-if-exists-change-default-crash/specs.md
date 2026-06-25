# Ownerless Column IF EXISTS Change Default Crash

## Problem

Ownerless column missing-`IF EXISTS` crash coverage covers representative
missing `MODIFY COLUMN IF EXISTS`, `RENAME COLUMN IF EXISTS`,
`CHANGE COLUMN IF EXISTS`, and `ALTER COLUMN IF EXISTS ... DEFAULT` no-op ALTER
branches. Those statements succeed as no-ops after MariaDB removes the missing
column operation, then still cross MyLite's ownerless dictionary-generation
boundary.

Recovery must preserve the existing table definition and data if a writer dies
after MariaDB accepts one of these no-op statements but before MyLite publishes
dictionary finish.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `CHANGE opt_column opt_if_exists_table_element field_ident field_spec
  opt_place`, setting `ALTER_CHANGE_COLUMN | ALTER_RENAME_COLUMN`, storing the
  old name in `Create_field::change`, and using the parsed `IF EXISTS` flag.
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER opt_column opt_if_exists_table_element field_ident SET DEFAULT ...`
  and `ALTER opt_column opt_if_exists_table_element field_ident DROP DEFAULT`
  through `LEX::add_alter_list()`.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes
  missing-column `CHANGE` and `MODIFY` entries from `Alter_info::create_list`
  and pushes `ER_BAD_FIELD_ERROR` as a note.
- The same function removes missing-column `ALTER COLUMN IF EXISTS` default
  entries from `Alter_info::alter_list` and clears
  `ALTER_CHANGE_COLUMN_DEFAULT` when no alter-list entries remain.
- `mariadb/libmariadb/include/mysqld_error.h` defines
  `ER_BAD_FIELD_ERROR` as 1054.
- MyLite `packages/libmylite/src/database.cc`
  `ownerless_finish_dictionary_ddl()` pauses at `dictionary-before-finish`
  after successful SQL execution and before publishing the ownerless dictionary
  generation.

## Scope And Non-Goals

In scope:

- Promote unsafe-hook selectors for missing-column `CHANGE COLUMN IF EXISTS`,
  `ALTER COLUMN IF EXISTS ... SET DEFAULT`, and
  `ALTER COLUMN IF EXISTS ... DROP DEFAULT` no-op crash recovery to
  metadata-only live-peer recovery.
- Verify recovery preserves the real `note` column metadata/default, leaves
  missing and attempted changed columns absent, keeps plain non-idempotent retry
  spellings returning errno 1054, and accepts later writes.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Real column change/default crash recovery, already covered by existing
  modify/rename/default selectors where applicable.
- The missing-column `CHANGE COLUMN IF EXISTS` and `ALTER COLUMN IF EXISTS
  SET/DROP DEFAULT` generated-column/CHECK expression variants are covered by
  `docs/specs/ownerless-column-if-exists-expression-crash/specs.md`.
- Period, partition, index, and foreign-key variants.
- SQL-level table-lock fault injection; prior representative SQL shapes did not
  reach the ownerless table-wait callback.
- External randomized DDL/RQG oracle execution.

## Design

Extend `mylite_ownerless_cross_process_sql_test` with three hook-only
selectors:

1. `dictionary-column-idempotent-change-crash`
   - Create an InnoDB table with `note VARCHAR(16) NOT NULL DEFAULT 'stable'`.
   - Kill a writer running
     `ALTER TABLE ... CHANGE COLUMN IF EXISTS missing_note changed_missing ...`
     at `dictionary-before-finish`.
   - Verify the real `note` column remains unchanged, both missing names remain
     absent, later writes work, and plain missing `CHANGE COLUMN` fails with
     1054.
2. `dictionary-column-idempotent-default-set-crash`
   - Kill a writer running
     `ALTER TABLE ... ALTER COLUMN IF EXISTS missing_note SET DEFAULT 'changed'`
     at `dictionary-before-finish`.
   - Verify the real `note` default remains `'stable'`, the missing column is
     absent, later default-backed writes use `'stable'`, and plain missing
     `ALTER COLUMN ... SET DEFAULT` fails with 1054.
3. `dictionary-column-idempotent-default-drop-crash`
   - Kill a writer running
     `ALTER TABLE ... ALTER COLUMN IF EXISTS missing_note DROP DEFAULT` at
     `dictionary-before-finish`.
   - Verify the real `note` default remains `'stable'`, the missing column is
     absent, later default-backed writes use `'stable'`, and plain missing
     `ALTER COLUMN ... DROP DEFAULT` fails with 1054.

Each selector holds another ownerless process live while the killed writer's
dictionary state is recovered by a new opener, verifies the native
file-operation marker stays clear, then rechecks state through ownerless and
native opens before and after `.shm` recreation.

## Compatibility Impact

No SQL feature behavior changes are intended. The slice extends ownerless ALTER
TABLE crash evidence for MariaDB missing-column `IF EXISTS` no-op semantics.

## Directory And Lifecycle Impact

No directory layout change. The tests exercise ownerless dictionary recovery,
volatile shared-memory rebuild, and native exclusive reopen against InnoDB
table metadata stored in the MyLite database directory.

## Native Storage Impact

No native storage format change. MariaDB removes the no-op ALTER entries before
InnoDB table-definition mutation; MyLite verifies recovery observes the
unchanged native table definition.

## Public API, Build, Size, License, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build the same target with `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-column-idempotent-change-crash`
  - `dictionary-column-idempotent-default-set-crash`
  - `dictionary-column-idempotent-default-drop-crash`
- Run standalone hook CTests for the same selectors.
- Run adjacent hook selectors for real column default/modify/rename and the
  prior missing-column IF EXISTS no-op crashes.
- Run production representative column-idempotent DDL coverage.
- Run `format-check`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- Focused selectors reach `dictionary-before-finish` and do not hang.
- Live-peer recovery succeeds while the held peer remains open.
- The native file-operation marker remains clear before and after recovery.
- Missing `CHANGE COLUMN IF EXISTS` recovery preserves original real-column
  metadata/default, keeps missing/changed names absent, and keeps plain missing
  change returning errno 1054.
- Missing `ALTER COLUMN IF EXISTS SET/DROP DEFAULT` recovery preserves the real
  column default, keeps the missing column absent, keeps plain missing default
  alterations returning errno 1054, and allows default-backed writes.
- Ownerless/native reopen before and after forced `.shm` rebuild observes the
  same final table state.

## Risks And Follow-Up

- This is deterministic missing-column no-op ALTER TABLE crash coverage, not an
  exhaustive idempotent table-element matrix.
- External randomized DDL oracle execution remains planned.
