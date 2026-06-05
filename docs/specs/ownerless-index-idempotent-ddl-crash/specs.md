# Ownerless Index Idempotent DDL Crash

## Problem

Ownerless index-idempotent DDL coverage verifies peer refresh for top-level
`CREATE INDEX IF NOT EXISTS` and `DROP INDEX IF EXISTS`. Ordinary hook-build
index crash coverage kills mutating secondary-index create/drop writers, but
the duplicate-create and missing-drop no-op paths still need deterministic
crash evidence.

This slice adds hook-build recovery evidence for top-level duplicate
idempotent secondary-index create and missing idempotent secondary-index drop.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level `CREATE INDEX` with
  `opt_if_not_exists` and top-level `DROP INDEX` with
  `opt_if_exists_table_element`.
- `mariadb/sql/sql_table.cc` maps top-level `CREATE INDEX` and `DROP INDEX`
  to `ALTER TABLE` execution.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes duplicate
  `ADD KEY IF NOT EXISTS` and missing `DROP INDEX IF EXISTS` work items before
  native execution, preserving the current table definition while returning
  success with diagnostics.
- `mariadb/libmariadb/include/mysqld_error.h` defines `ER_DUP_KEYNAME` as
  errno 1061 for plain duplicate index creation.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `CREATE` and `DROP` as
  ownerless dictionary DDL and exposes the unsafe `dictionary-before-finish`
  hook after native SQL execution but before ownerless dictionary finish.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-index-idempotent-create-crash` creates an InnoDB table and a
  secondary index over `value`, then kills a writer after duplicate
  `CREATE INDEX IF NOT EXISTS` for the same index name over `note` reaches
  `dictionary-before-finish`.
- `dictionary-index-idempotent-drop-crash` creates an InnoDB table and a
  secondary index over `value`, then kills a writer after `DROP INDEX IF
  EXISTS` for a missing index name reaches `dictionary-before-finish`.

Both selectors keep a live ownerless peer open while the writer is killed,
prove cleanup remains busy until no-live recovery, then verify ownerless and
ordinary native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate top-level
  `CREATE INDEX IF NOT EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for missing top-level
  `DROP INDEX IF EXISTS` no-op behavior.
- Index key-part preservation for the real secondary index.
- Absence of missing-index metadata after no-op drop recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and
  `ALTER TABLE ... DROP INDEX IF EXISTS` crash variants.
- Unique, primary, full-text, spatial, generated-column, prefix, direction, and
  online-option idempotent index crash variants.
- SQL-level table-lock fault injection for native table-wait paths.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent secondary-index migration statements by proving a
dead writer in MyLite's dictionary publication window does not replace the
existing index key part, drop the real index, create the missing index, or
leave stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise native InnoDB secondary-index
metadata inside the table's native files, ownerless live-peer cleanup blocking,
no-live recovery, forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The preserved tables are InnoDB. MyLite does not reinterpret native metadata;
it coordinates the ownerless dictionary boundary and verifies durable reopen
behavior for the preserved native index metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-index-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-index-idempotent-drop-crash`
- Run normal `index-idempotent-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Duplicate idempotent create recovery keeps the original `value` index key
  part, leaves the attempted `note` replacement absent, and keeps plain
  duplicate create returning errno 1061.
- Missing idempotent drop recovery keeps the real index usable and the missing
  index absent.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic top-level no-op secondary-index DDL crash recovery,
  not every idempotent index spelling.
- `ALTER TABLE` secondary-index idempotent no-op crash recovery is covered by
  `docs/specs/ownerless-alter-index-idempotent-ddl-crash/specs.md`.
- Unique-index no-op crash recovery is covered by
  `docs/specs/ownerless-unique-index-idempotent-ddl-crash/specs.md`.
- Online-option matrices and broader randomized DDL oracle execution remain
  planned.
