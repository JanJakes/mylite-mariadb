# Ownerless Generated Column Success DDL Crash

## Problem Statement

Ownerless generated-column coverage already proves peer refresh for
create-time generated columns, generated-column ALTERs, and generated-column
secondary-index DDL. Hook coverage also proves failed generated-column DDL
does not leave rejected metadata behind when a writer dies before ownerless
dictionary finish.

The remaining generated-column crash gap is the successful DDL boundary: a
writer can finish MariaDB's native generated-column DDL, then die before MyLite
publishes the ownerless dictionary generation. MyLite must recover the native
table, generated-column metadata, generated values, and generated-column index
metadata without requiring the crashed process to finish.

Status note: a later ownerless dictionary crash CI attribution audit found this
selector was stale: successful generated-column `ALTER TABLE ... ADD COLUMN`
was not classified as a recoverable ownerless dictionary DDL when the generated
column definition used a comma-separated multi-ADD clause. This slice fixes
that classifier gap, proves live recovery, verifies marker retention only for
the native file-lifecycle cases that still need it, and registers the selector
as a standalone hook CTest.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:3799-3805` calls
  `Column_definition::check_vcol_for_key()` for unique, ordinary secondary, and
  foreign-key key parts.
- `mariadb/sql/field.cc:10800-10827` validates generated-column expressions
  through `Item::check_vcol_func_processor` before native storage sees the table
  definition.
- `mariadb/sql/sql_table.cc:10681-10705` documents and defines
  `mysql_alter_table()`, including the `CREATE|DROP INDEX` mapping and
  field/index table-definition rebuild behavior.
- `mariadb/sql/sql_table.cc:11568-11725` creates the altered table definition,
  fills `Alter_inplace_info`, prepares an altered table object, and asks the
  handler whether in-place ALTER is supported.
- `mariadb/storage/innobase/handler/handler0alter.cc:1502-1527` detects indexed
  virtual columns, while `handler0alter.cc:3825-3972` builds InnoDB index
  definitions and generated-column index field metadata.
- `packages/libmylite/src/database.cc:2939-3007` begins ownerless dictionary
  DDL before executing direct SQL, finishes it after successful MariaDB
  execution, and marks native file-operation checkpoint evidence when needed.
- `packages/libmylite/src/database.cc:9254-9332` implements
  `ownerless_begin_dictionary_ddl()` and `ownerless_finish_dictionary_ddl()`.
  The unsafe `dictionary-before-finish` hook kills a writer after MariaDB has
  returned from native DDL but before MyLite marks the shared dictionary state
  idle.
- `packages/libmylite/src/database.cc` classifies ownerless `ALTER TABLE ...
  ADD COLUMN` recovery before executing the MariaDB statement. Generated-column
  ADD recovery must therefore accept deterministic generated-column definition
  tokens and comma-separated multi-ADD clauses so the pre-finish recovery kind
  is durable before the crash hook fires.
- Existing hook selectors for secondary-index, column, view, trigger, and
  failed generated-column DDL use the same live-peer recovery or no-live drain
  boundary around `dictionary-before-finish`.

## Design

Add a hook-only selector,
`dictionary-generated-column-success-crash`, to
`mylite_ownerless_cross_process_sql_test`.

The selector uses one ownerless database and three killed writers:

1. Successful `CREATE TABLE` with stored and virtual generated columns. After
   live recovery, verify `.frm` and `.ibd` files exist, generated-column
   metadata is present, inserts omitting generated columns work, generated
   values are correct, and the native file-operation marker remains until the
   final no-live drain.
2. Successful `ALTER TABLE ... ADD COLUMN` adding stored and virtual generated
   columns to an existing InnoDB table through a comma-separated multi-ADD
   clause. After live recovery, verify existing rows expose generated values
   and later inserts recompute them. The crash must publish the native
   file-operation marker, but live recovery may clear it once native proof is
   complete.
3. Successful `CREATE INDEX` over a virtual generated column. After recovery,
   verify `INFORMATION_SCHEMA.STATISTICS`, `FORCE INDEX` reads, later DML, and
   native file-operation marker retention until the final no-live drain.

Each killed writer runs with a live ownerless peer so cleanup must remain busy
until no-live recovery. The final state is verified through ownerless reopen,
ordinary native exclusive reopen, forced `.shm` rebuild, and native exclusive
reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for successful generated-column
  `CREATE TABLE`,
- crash-at-`dictionary-before-finish` coverage for successful generated-column
  `ALTER TABLE ... ADD COLUMN`,
- crash-at-`dictionary-before-finish` coverage for generated-column secondary
  index creation,
- ownerless cleanup-busy behavior while a live peer remains,
- ownerless/native reopen before and after forced shared-memory rebuild.

Out of scope:

- exhaustive generated-column expression and online-option matrices,
- generated-column foreign-key crash injection beyond the representative
  ADD CONSTRAINT coverage in
  `docs/specs/ownerless-generated-column-foreign-key-crash/specs.md`,
- failed generated-column validation, already covered by
  `ownerless-generated-column-failed-ddl-crash`,
- SQL-level table-lock fault injection for native table-wait paths,
- external MariaDB/RQG generated-column stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens the ownerless
generated-column claim by proving representative successful generated-column
DDL remains recoverable when the writer dies at MyLite's dictionary publication
boundary.

This does not make generated-column crash coverage exhaustive; broader
generated-column foreign-key and randomized oracle coverage remain planned.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test verifies the successful
generated-column create path leaves native table files under `datadir/app/`:

- `ownerless_generated_success_crash_create.frm`,
- `ownerless_generated_success_crash_create.ibd`.

The selector exercises the existing ownerless process-slot cleanup,
dictionary-generation recovery, `.shm` rebuild, and ordinary native exclusive
reopen lifecycle.

## Native Storage Impact

No InnoDB file format changes. MyLite continues to rely on MariaDB/InnoDB
native generated-column and generated-column index metadata; the slice only
proves ownerless recovery rebuilds volatile coordination around completed
native metadata changes.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-generated-column-success-crash`.
- Run adjacent generated-column hook coverage:
  `dictionary-generated-column-failed-crash`.
- Run adjacent non-hook generated-column selectors:
  `generated-column-alter`, `generated-column-index-ddl`, and
  `generated-column-indexed-expression`.
- Run the hook crash-tail selector or ownerless hook CTest subset when time
  permits.
- Run ownerless stress, `format-check`, `tidy`, and `git diff --check`.

## Acceptance Criteria

- Each successful DDL writer reaches `dictionary-before-finish` and can be
  killed without hanging the test.
- Live ownerless recovery succeeds while another ownerless peer remains open;
  file-lifecycle cases retain the native file-operation marker until final
  no-live drain, while generated-column ALTER recovery may clear it after native
  proof completes.
- Recovered generated-column `CREATE TABLE` metadata, native files, inserts,
  and generated values are correct.
- Recovered generated-column `ALTER TABLE` metadata and generated values are
  correct for existing and later rows.
- Recovered generated-column secondary-index metadata is present and
  `FORCE INDEX` reads work before and after later DML.
- Ownerless and ordinary native reopen observe the same final state before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic generated-column DDL crash coverage, not randomized
  crash fuzzing.
- Generated-column foreign-key drop/action crash injection remains planned.
  Representative generated-column FK ADD CONSTRAINT crash recovery is covered
  by `docs/specs/ownerless-generated-column-foreign-key-crash/specs.md`.
- Full external MariaDB/RQG generated-column stress remains environment-owned
  follow-up work.
