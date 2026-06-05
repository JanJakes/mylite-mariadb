# Ownerless ALTER Index Idempotent DDL Crash

## Problem

Ownerless ALTER index idempotent DDL coverage verifies peer refresh for
`ALTER TABLE ... ADD INDEX IF NOT EXISTS` and
`ALTER TABLE ... DROP INDEX IF EXISTS`, including duplicate-add and missing-drop
no-op behavior. Hook-build index-idempotent crash coverage already covers the
top-level `CREATE INDEX IF NOT EXISTS` and `DROP INDEX IF EXISTS` no-op
spellings, but not the equivalent ALTER-table table-element spellings.

This slice adds hook-build recovery evidence for duplicate idempotent ALTER
index add and missing idempotent ALTER index drop.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:key_def` parses
  `key_or_index opt_if_not_exists ...` and stores the idempotency flag through
  `Lex->add_key()`.
- `mariadb/sql/sql_yacc.yy:alter_list_item` parses
  `DROP key_or_index opt_if_exists_table_element field_ident` into an
  `Alter_drop` with the `IF EXISTS` bit.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes duplicate
  `ADD KEY IF NOT EXISTS` and missing `DROP INDEX IF EXISTS` work items before
  native execution, preserving the current table definition while returning
  success with diagnostics.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
  dictionary DDL and exposes the unsafe `dictionary-before-finish` hook after
  native SQL execution but before ownerless dictionary finish.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-alter-index-idempotent-create-crash` creates an InnoDB table and
  a secondary index through `ALTER TABLE ... ADD INDEX`, then kills a writer
  after duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` for the same index
  name over a different key part reaches `dictionary-before-finish`.
- `dictionary-alter-index-idempotent-drop-crash` creates an InnoDB table and a
  secondary index through `ALTER TABLE ... ADD INDEX`, then kills a writer
  after `ALTER TABLE ... DROP INDEX IF EXISTS` for a missing index name reaches
  `dictionary-before-finish`.

Both selectors keep a live ownerless peer open while the writer is killed,
prove cleanup remains busy until no-live recovery, then verify ownerless and
ordinary native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate
  `ALTER TABLE ... ADD INDEX IF NOT EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for missing
  `ALTER TABLE ... DROP INDEX IF EXISTS` no-op behavior.
- Index key-part preservation for the real secondary index.
- Absence of missing-index metadata after no-op drop recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Top-level index idempotent crash recovery, already covered by
  `docs/specs/ownerless-index-idempotent-ddl-crash/specs.md`.
- Unique, primary, full-text, spatial, generated-column, prefix, direction, and
  online-option ALTER index idempotent crash variants.
- SQL-level table-lock fault injection and external randomized DDL/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent ALTER index DDL by proving a dead writer in
MyLite's dictionary publication window does not replace the existing key part,
drop the real index, create the missing index, or leave stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise native InnoDB secondary-index
metadata inside the table's native files, ownerless live-peer cleanup blocking,
no-live recovery, forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The preserved tables are InnoDB. MyLite does not reinterpret native metadata;
it coordinates the ownerless dictionary boundary and verifies durable reopen
behavior for the preserved native ALTER-created secondary-index metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-alter-index-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-alter-index-idempotent-drop-crash`
- Run normal `index-idempotent-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Duplicate ALTER idempotent create recovery keeps the original `value` index
  key part, leaves the attempted `note` replacement absent, and keeps plain
  duplicate ALTER add returning errno 1061.
- Missing ALTER idempotent drop recovery keeps the real index usable and the
  missing index absent.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic ALTER-table no-op secondary-index DDL crash
  recovery, not every idempotent index spelling.
- Unique-index no-op crash variants, online-option matrices, and broader
  randomized DDL oracle execution remain planned.
