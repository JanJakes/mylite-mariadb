# Ownerless Unique Index Idempotent DDL Crash

## Problem

Ownerless unique-index idempotent DDL coverage verifies peer refresh for
top-level `CREATE UNIQUE INDEX IF NOT EXISTS` and
`ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS`, including duplicate no-op
preservation of the active unique key. Existing hook-build crash coverage
covers non-unique secondary-index idempotent no-op branches and
`CREATE OR REPLACE UNIQUE INDEX`, but not duplicate unique-index no-op writers
killed after MariaDB success and before MyLite ownerless dictionary finish.

This slice adds deterministic hook-build recovery evidence for those unique
secondary-index no-op branches.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level
  `CREATE [OR REPLACE] UNIQUE INDEX opt_if_not_exists` and calls
  `Lex->add_create_index(Key::UNIQUE, ...)`, carrying the idempotency bit into
  the alter-table execution path.
- `mariadb/sql/sql_yacc.yy:key_def` parses table-element unique keys through
  `constraint_key_type opt_if_not_exists` and `Lex->add_key()`, covering
  `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` iterates
  `alter_info->key_list`, detects duplicate key names for keys with
  `if_not_exists()`, pushes `ER_DUP_KEYNAME` as a note, removes the work item,
  and preserves the current key definition. This path is shared by non-unique,
  unique, and primary key objects; primary-key no-op crash recovery is covered
  separately.
- `packages/libmylite/src/database.cc` classifies `CREATE` and `ALTER` as
  ownerless dictionary DDL, starts ownerless dictionary publication before
  native execution, and exposes the unsafe `dictionary-before-finish` hook after
  native SQL execution but before publishing the stable dictionary generation.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-unique-index-idempotent-create-crash` creates an InnoDB table and
  a unique secondary index with top-level `CREATE UNIQUE INDEX`, then kills a
  writer after duplicate `CREATE UNIQUE INDEX IF NOT EXISTS` for the same index
  name over a different key part reaches `dictionary-before-finish`.
- `dictionary-alter-unique-index-idempotent-create-crash` creates an InnoDB
  table and a unique secondary index with
  `ALTER TABLE ... ADD UNIQUE INDEX`, then kills a writer after duplicate
  `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` for the same index name over
  a different key part reaches `dictionary-before-finish`.

Both selectors keep a live ownerless peer open while the writer is killed,
prove cleanup remains busy until no-live recovery, then verify ownerless and
ordinary native reopen before and after forced `.shm` rebuild. The post-crash
checks verify the original unique key part is still enforced, the attempted
replacement key part remains absent/non-unique, plain duplicate key-name DDL
still returns errno 1061, post-recovery writes succeed, and forced-index reads
observe the preserved native InnoDB unique index.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate top-level
  `CREATE UNIQUE INDEX IF NOT EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for duplicate
  `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op behavior.
- Unique key-part preservation and duplicate-key enforcement for the real
  unique secondary index.
- Absence/non-enforcement of the attempted replacement key part.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Non-unique idempotent secondary-index no-op crash recovery, already covered
  by `docs/specs/ownerless-index-idempotent-ddl-crash/specs.md` and
  `docs/specs/ownerless-alter-index-idempotent-ddl-crash/specs.md`.
- Primary-key idempotent no-op crash recovery, already covered by
  `docs/specs/ownerless-primary-key-idempotent-ddl-crash/specs.md`.
- Unique index missing-drop crash variants; missing `DROP INDEX IF EXISTS` is
  not unique-specific and is covered by the secondary-index idempotent crash
  selectors.
- Prefix, direction, generated-column, full-text, spatial, online-option, and
  randomized DDL oracle variants.
- SQL-level table-lock fault injection and external RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent unique secondary-index DDL by proving a dead
writer in MyLite's dictionary publication window does not replace the existing
unique key definition or leave stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise native InnoDB secondary-index
metadata inside the table's native files, ownerless live-peer cleanup blocking,
no-live recovery, forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The preserved tables are InnoDB. MyLite does not reinterpret native metadata;
it coordinates the ownerless dictionary boundary and verifies durable reopen
behavior for preserved native unique secondary-index metadata and enforcement.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-unique-index-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-alter-unique-index-idempotent-create-crash`
- Run normal `unique-index-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Duplicate top-level unique-index create recovery keeps the original unique
  key definition, keeps plain duplicate create returning errno 1061, rejects
  duplicate values for the original key, and allows values that would only
  collide on the attempted replacement key.
- Duplicate ALTER unique-index add recovery keeps the original unique key
  definition, keeps plain duplicate ALTER-add returning errno 1061, rejects
  duplicate values for the original key, and allows values that would only
  collide on the attempted replacement key.
- Ownerless and ordinary native reopen observe the same rows, metadata, and
  unique enforcement before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic duplicate unique secondary-index no-op crash
  recovery, not every idempotent unique-index spelling.
- Prefix, direction, generated-column, online-option matrices, and broader
  randomized DDL oracle execution remain planned.
