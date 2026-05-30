# Ownerless Charset Convert DDL Refresh

## Problem

Ownerless DDL coverage includes representative column-shape changes, generated
columns, indexes, foreign keys, CHECK constraints, and instant-column metadata.
Application schemas also commonly run table-wide character-set conversion with
`ALTER TABLE ... CONVERT TO CHARACTER SET`. That path updates table and column
metadata and may rebuild native InnoDB storage. Ownerless mode needs focused
evidence that an already-open peer refreshes the converted column metadata and
that the final table survives reopen and shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... CONVERT TO CHARACTER SET ... COLLATE ...` through the
  `CONVERT_SYM TO_SYM charset` alter-table grammar.
- `mariadb/sql/sql_table.cc` resolves convert/default charset and collation
  attributes during ALTER TABLE preparation, including
  `alter_table_convert_to_charset`.
- MyLite ownerless DDL already serializes DDL with the shared dictionary
  generation, refreshes peers before they use changed metadata, and retains
  page-version/recovery evidence until native checkpoint proof permits reclaim.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for
  `ALTER TABLE ... CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci`.
- Verify an already-open peer sees the original `latin1` column metadata before
  conversion and the `utf8mb4` metadata after conversion through
  `information_schema.columns`.
- Verify existing rows remain readable and the already-open peer can insert a
  new row after the conversion boundary.
- Verify the final converted table survives ownerless reopen, ordinary
  exclusive reopen, forced `.shm` rebuild, and exclusive reopen after rebuild.
- Do not claim every charset/collation pair, index prefix-width edge case, or
  external MariaDB/RQG DDL stress case.

## Design

Add a `charset-convert-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table with
   `DEFAULT CHARSET=latin1 COLLATE=latin1_swedish_ci`, inserts two rows, and
   signals the parent.
2. The already-open parent verifies the `name` column reports `latin1` metadata
   and reads the inserted rows.
3. The child runs
   `ALTER TABLE app.ownerless_charset_convert_base CONVERT TO CHARACTER SET
   utf8mb4 COLLATE utf8mb4_general_ci`.
4. The parent verifies the converted `utf8mb4` metadata, reads the original
   rows, inserts another row, and checks final aggregates.
5. Reopen helper assertions verify the converted metadata and rows through
   ownerless read/write, ordinary exclusive read/write, forced `.shm` rebuild,
   and ordinary exclusive read/write after rebuild.

## Compatibility Impact

No public C API behavior changes. The slice strengthens partial ownerless
DDL/dictionary compatibility for a common table-rebuild metadata operation while
leaving broad DDL/file-lifecycle recovery and external oracle stress planned.

## Directory And Lifecycle Impact

No new files or layout are introduced. The test exercises existing native InnoDB
table files and ownerless concurrency metadata inside the MyLite-owned database
directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. The test verifies MyLite's
ownerless dictionary boundary and retained recovery evidence keep the converted
native table usable for peers and later reopens.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `charset-convert-ddl` in `embedded-dev`.
- Build and run focused `charset-convert-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The already-open peer observes pre-conversion `latin1` metadata.
- The already-open peer observes post-conversion `utf8mb4` metadata.
- Existing rows remain readable and post-conversion peer DML persists.
- The converted table survives ownerless/native reopen before and after forced
  `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This does not cover index-prefix length changes caused by wider charsets,
  every collation family, or randomized DDL oracles. Those remain separate
  compatibility gaps.
