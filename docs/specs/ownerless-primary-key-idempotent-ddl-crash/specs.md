# Ownerless Primary-Key Idempotent DDL Crash

## Problem

Ownerless primary-key idempotent DDL coverage verifies that
`ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` preserves an existing primary
key for already-open peers. Ordinary hook-build primary-key crash coverage kills
a mutating primary-key replacement writer, but the duplicate-add no-op path
still needs deterministic crash evidence.

This slice adds hook-build recovery evidence for duplicate idempotent
primary-key add.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:key_def` parses
  `constraint_key_type opt_if_not_exists` through `Lex->add_key()`.
- `mariadb/sql/sql_yacc.yy:constraint_key_type` maps `PRIMARY KEY` to
  `Key::PRIMARY`.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes duplicate
  `ADD PRIMARY KEY IF NOT EXISTS` work when a primary key already exists,
  preserving the current primary key while returning success with diagnostics.
- `mariadb/sql/sql_yacc.yy:alter_list_item` parses bare `DROP PRIMARY KEY`
  without an `IF EXISTS` branch, so this slice does not claim idempotent
  primary-key drop coverage.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
  dictionary DDL and exposes the unsafe `dictionary-before-finish` hook after
  native SQL execution but before ownerless dictionary finish.

## Design

Add one unsafe-hook selector to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-primary-key-idempotent-crash` creates an InnoDB table with
  `PRIMARY(id)` and a non-unique `code` candidate column, then kills a writer
  after `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS (code)` reaches
  `dictionary-before-finish`.

The selector now uses the held-live-peer crash helper and
`ownerless-primary-key-idempotent-live-recovery` metadata proof to recover
while another ownerless process is live with the native file-operation marker
clear, then verifies ownerless and ordinary native reopen before and after
forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate
  `ADD PRIMARY KEY IF NOT EXISTS` no-op behavior.
- Preservation of `PRIMARY(id)` metadata and duplicate-key enforcement.
- Absence of `PRIMARY(code)` metadata after no-op recovery.
- Proof that `code` remains non-unique by accepting a duplicate `code` value
  with a new `id`.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `DROP PRIMARY KEY IF EXISTS`, which is not a MariaDB grammar branch for this
  base.
- Mutating primary-key replacement crash recovery, already covered by
  `docs/specs/ownerless-primary-key-ddl-crash/specs.md`.
- Composite, descending, AUTO_INCREMENT, generated-column, foreign-key, and
  algorithm/lock-option idempotent primary-key crash variants.
- SQL-level table-lock fault injection and external randomized DDL/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent primary-key DDL by proving a dead writer in
MyLite's dictionary publication window does not replace the existing clustered
key, make the candidate column unique, or leave stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB primary-key
metadata inside the table's native files, ownerless dictionary live recovery,
forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The preserved table is InnoDB. MyLite does not synthesize clustered-index
metadata; it coordinates the ownerless dictionary boundary and verifies durable
reopen behavior for the preserved native primary key.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-primary-key-idempotent-crash`
- Run normal `primary-key-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Duplicate primary-key no-op recovery completes while a peer remains live.
- Duplicate idempotent ADD-primary recovery keeps `PRIMARY(id)`, leaves
  `PRIMARY(code)` absent, and keeps plain duplicate primary-key add returning
  errno 1068.
- Duplicate `id` inserts fail, while duplicate `code` inserts with a new `id`
  succeed.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic duplicate ADD-primary no-op crash recovery, not the
  full primary-key option matrix.
- Composite, descending, AUTO_INCREMENT, generated-column, foreign-key,
  algorithm/lock-option, and broader randomized DDL oracle execution remain
  planned.
