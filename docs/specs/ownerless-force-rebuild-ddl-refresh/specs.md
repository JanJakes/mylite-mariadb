# Ownerless Force Rebuild DDL Refresh

## Problem

Ownerless DDL coverage includes many visible metadata changes, but MariaDB also
supports `ALTER TABLE ... FORCE` to request an identical table rebuild. That
path exercises native DDL rebuild and file-lifecycle machinery without a
schema shape change that peers can observe directly. Ownerless mode needs
evidence that an already-open peer can continue using the rebuilt InnoDB table
and secondary index after the dictionary boundary, and that the final state
survives reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... FORCE` and sets
  `ALTER_RECREATE`.
- `mariadb/sql/sql_table.cc` treats `ALTER TABLE ... FORCE` without other
  options as `recreate_identical_table`, sharing logic with repair/optimize
  rebuild paths.
- `mariadb/storage/innobase/handler/handler0alter.cc` includes
  `ALTER_RECREATE_TABLE` in InnoDB rebuild operations.
- MyLite ownerless DDL marks the directory-backed dictionary generation active
  before MariaDB executes DDL and publishes a stable generation after success;
  peers refresh table metadata before the next statement that uses changed
  dictionary state.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for `ALTER TABLE ... FORCE` over an
  InnoDB table with a secondary index.
- Verify an already-open ownerless peer reads through the secondary index
  before and after the rebuild boundary.
- Verify the already-open peer can insert after the rebuild boundary.
- Verify final native metadata, secondary-index metadata, and rows survive
  ownerless reopen, ordinary exclusive reopen, forced `.shm` rebuild, and
  exclusive reopen after rebuild.
- Do not claim full DDL/file-lifecycle recovery, partitioned tables, or
  external randomized DDL oracle coverage.

## Design

Add a `force-rebuild-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table with a secondary index,
   inserts rows, and signals the parent.
2. The already-open parent verifies positive InnoDB native metadata and
   forced-index reads.
3. The child runs `ALTER TABLE ... FORCE`.
4. The parent verifies the table remains present in InnoDB native metadata,
   reads rows through the secondary index, inserts a row, and checks final
   aggregates.
5. Reopen helper assertions verify the table, secondary index, and rows through
   ownerless/native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

No public API behavior changes. The slice strengthens partial ownerless
DDL/dictionary compatibility for an InnoDB rebuild request while leaving full
DDL/file-lifecycle recovery and external oracle stress planned.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing native InnoDB table
files and ownerless concurrency files under the MyLite-owned database directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite coordinates the ownerless
DDL boundary and verifies durable reopen behavior for the rebuilt table and
secondary index.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `force-rebuild-ddl` in `embedded-dev`.
- Build and run focused `force-rebuild-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The already-open peer reads through the secondary index before the rebuild.
- The already-open peer reads through the secondary index after the rebuild.
- Post-rebuild peer DML persists.
- Final native metadata, secondary-index metadata, and rows survive
  ownerless/native reopen before and after forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This does not prove every DDL-created or DDL-dropped tablespace replay path.
- External MariaDB/RQG DDL oracles remain planned.
