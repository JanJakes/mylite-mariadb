# Ownerless Pressure Generated-Column Policy

## Problem

Ownerless active-reader pressure coverage already throttles representative DML,
table/schema/view/trigger DDL, column ALTER variants, constraint DDL, and
storage/rebuild ALTERs while a repeatable-read snapshot pin retains page-version
WAL at the configured soft limit. Generated-column DDL is a separate supported
metadata family because MariaDB validates generated expressions, stores
generated-column metadata in the table definition, and lets deterministic
stored and virtual generated columns participate in secondary indexes.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE`,
  `SQLCOM_CREATE_INDEX`, and `SQLCOM_DROP_INDEX` with `CF_CHANGES_DATA`, so
  generated-column ALTER and generated-column index DDL are write statements in
  MariaDB's command model.
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE` into `SQLCOM_ALTER_TABLE` and
  top-level `DROP INDEX ... ON table` into `SQLCOM_DROP_INDEX`.
- `mariadb/sql/sql_table.cc` validates generated-column key parts through
  `Column_definition::check_vcol_for_key()` for ordinary, unique, and foreign
  key preparation paths.
- `mariadb/storage/innobase/handler/ha_innodb.cc` builds templates for stored
  and virtual generated columns so InnoDB can read and index their values.
- `packages/libmylite/src/database.cc` runs
  `enforce_ownerless_page_log_limit_policy()` before ownerless statement locks,
  dictionary refresh, and MariaDB execution. The pressure test must therefore
  prove generated-column ALTER and index DDL return `MYLITE_BUSY` before native
  metadata changes when retained WAL is already at the configured limit.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with:
  - `ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS ... STORED`,
  - `ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS ... VIRTUAL`,
  - `CREATE INDEX ... ON ... (virtual_generated_column)`, and
  - `DROP INDEX ... ON ...` for an index over a generated column.
- Verify those statements return `MYLITE_BUSY` while a live snapshot pin keeps
  page-version WAL at the configured limit.
- Verify blocked statements leave generated-column metadata and generated-index
  metadata unchanged.
- Verify the same statements succeed after the reader releases, generated
  values are recalculated from base columns, the generated-column index is
  usable with `FORCE INDEX`, and final state survives ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Production pressure classifier changes.
- Exhaustive generated-column expression, primary-key rejection, or
  online-option matrices.
- Generated-column crash fault injection.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized pressure stress.

## Design

Reuse the retained-WAL setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create one baseline table without generated columns and one table with
   stored and virtual generated columns plus an existing generated-column
   secondary index.
2. Hold a repeatable-read snapshot in a peer ownerless process.
3. Commit one ownerless update so page-version WAL remains retained by the
   reader pin.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Assert generated-column ALTER, generated-column index creation, and
   generated-column index drop return `MYLITE_BUSY`.
6. Assert blocked state has no new generated columns, keeps the original
   generated-column index, and has not created the new virtual-column index.
7. Release the reader, execute the same statements successfully, and verify
   generated-column values, generated-index metadata, and forced-index reads.
8. Verify the final generated-column and index state through ownerless/native
   reopen before and after forced shared-memory rebuild.

## Compatibility Impact

No SQL semantics change. The slice adds deterministic evidence that supported
generated-column ALTER and generated-column secondary-index DDL are
pressure-throttled before MariaDB mutates native metadata while retained
page-version WAL is over the configured ownerless limit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises existing InnoDB table
metadata/files, ownerless dictionary generation, page-version WAL retention,
checkpointing, and forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must not reach native generated
column or generated-index metadata mutation paths. After pressure clears,
MariaDB's native ALTER TABLE, CREATE INDEX, and DROP INDEX machinery remains
responsible for generated-column metadata and secondary-index storage.

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
- Run the ownerless SQL CTest shard containing the selector in both presets.
- Run adjacent active-reader pressure stress coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Generated-column ALTER and generated-column index DDL return `MYLITE_BUSY`
  with the pressure-limit diagnostic while retained WAL is at the configured
  limit.
- Blocked statements leave generated-column and generated-index metadata
  unchanged.
- After the reader releases, the same statements succeed, generated values and
  forced-index reads are correct, and final state survives ownerless/native
  reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is deterministic generated-column pressure coverage, not exhaustive
  generated-column DDL coverage.
- Stored generated-column foreign-key pressure is covered by
  `docs/specs/ownerless-pressure-generated-column-fk-policy/specs.md`.
  Broader generated-column expression variants, crash injection, and external
  randomized pressure stress remain separate work.
