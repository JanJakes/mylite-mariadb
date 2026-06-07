# Ownerless Pressure Constraint DDL Policy

## Problem

Ownerless active-reader pressure throttling now covers representative DML,
table DDL, schema/view/trigger variants, CTAS post-create DML, and column ALTER
variants. CHECK and FOREIGN KEY constraints remain a bounded supported DDL
family with distinct MariaDB metadata and InnoDB dictionary effects. The
pressure selector should prove those add/drop paths are stopped before side
effects while retained page-version WAL is already at the configured limit.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE` with
  `CF_CHANGES_DATA`.
- `mariadb/sql/sql_yacc.yy` parses `ADD CONSTRAINT ... CHECK`,
  `DROP CONSTRAINT`, `ADD CONSTRAINT ... FOREIGN KEY`, and
  `DROP FOREIGN KEY` into the shared ALTER TABLE parse structures.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` normalizes CHECK and
  FOREIGN KEY add/drop flags, and `mysql_alter_table()` executes the common
  ALTER TABLE path.
- `mariadb/storage/innobase/handler/handler0alter.cc` handles
  `ALTER_DROP_CHECK_CONSTRAINT` and `ALTER_DROP_FOREIGN_KEY` as native InnoDB
  ALTER flags; FK add/drop also updates InnoDB dictionary metadata.
- `packages/libmylite/src/database.cc` runs
  `enforce_ownerless_page_log_limit_policy()` before ownerless statement
  locking, dictionary refresh, or MariaDB execution. The test must therefore
  prove supported constraint ALTER statements return `MYLITE_BUSY` and leave
  metadata untouched at the pressure boundary.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with:
  - `ALTER TABLE ... ADD CONSTRAINT ... CHECK`,
  - `ALTER TABLE ... DROP CONSTRAINT`,
  - `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY`, and
  - `ALTER TABLE ... DROP FOREIGN KEY`.
- Verify each statement returns `MYLITE_BUSY` while a repeatable-read peer pin
  retains page-version WAL at the configured soft limit.
- Verify blocked statements leave `INFORMATION_SCHEMA.CHECK_CONSTRAINTS` and
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS` unchanged.
- Verify the same statements succeed after the reader releases and that CHECK
  enforcement, dropped-CHECK release, FK enforcement, and dropped-FK release are
  observable before final reopen checks.

Out of scope:

- Exhaustive FK action matrices, cyclic FK graphs, generated-column FKs, CHECK
  expression variants, or randomized external RQG stress.
- Production pressure classifier changes.
- SQL table-lock fault injection.

## Design

Reuse the retained-WAL setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create dedicated baseline tables for CHECK add, CHECK drop, FK add, and FK
   drop coverage.
2. Hold a repeatable-read snapshot in a peer ownerless process.
3. Commit one ownerless update so page-version WAL remains retained by the
   active reader.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Assert each constraint ALTER spelling returns `MYLITE_BUSY`.
6. Assert added constraints are absent and drop-target constraints are still
   present.
7. Release the reader, run the same statements successfully, and prove:
   - the added CHECK rejects invalid values,
   - the dropped CHECK allows a formerly invalid value,
   - the added FK rejects an orphan child, and
   - the dropped FK allows an orphan child.
8. Verify final metadata and row aggregates through ownerless/native reopen
   before and after forced shared-memory rebuild.

## Compatibility Impact

No SQL behavior changes. The slice adds evidence that supported CHECK and
FOREIGN KEY metadata-changing ALTER statements are pressure-throttled before
native metadata or row state can change while retained WAL is over the
configured ownerless limit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises existing InnoDB table
metadata/files, ownerless dictionary generation, page-version WAL retention,
checkpointing, and forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must not reach native InnoDB
constraint metadata mutation paths. After pressure clears, MariaDB's native
ALTER TABLE machinery remains responsible for constraint metadata and
enforcement.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `active-reader-pressure-write-policy` selector in
  `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL shard or subset containing the selector.
- Run adjacent ownerless active-reader pressure stress.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Constraint add/drop ALTER statements return `MYLITE_BUSY` with the
  pressure-limit diagnostic while active-reader WAL pressure is at the
  configured limit.
- Blocked statements leave CHECK and FK metadata unchanged.
- After the reader releases, the same statements succeed and final
  metadata/enforcement survives ownerless/native reopen before and after forced
  `.shm` rebuild.

## Risks And Follow-Up

- This is representative deterministic SQL coverage, not an exhaustive
  constraint matrix.
- Broader generated-column FK, cyclic FK, CHECK-expression, and randomized
  external oracle pressure combinations remain planned.
