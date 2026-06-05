# Ownerless Primary Key Idempotent DDL

## Problem

Ownerless primary-key DDL coverage proves clustered-index replacement from one
process to an already-open peer. MariaDB also accepts
`ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS`, which should be a no-op when a
primary key already exists. MyLite needs bounded evidence that this no-op branch
preserves the current clustered key for already-open ownerless peers before the
same selector performs the existing replacement.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:key_def` parses
  `constraint_key_type opt_if_not_exists` through `Lex->add_key()`.
- `mariadb/sql/sql_yacc.yy:constraint_key_type` maps `PRIMARY KEY` to
  `Key::PRIMARY`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` detects an existing
  primary key when `key->type == Key::PRIMARY`, removes duplicate
  `ADD PRIMARY KEY IF NOT EXISTS` before native execution, and preserves the
  current primary key.
- `mariadb/sql/sql_yacc.yy:alter_list_item` parses bare `DROP PRIMARY KEY`
  without an `IF EXISTS` branch, so this slice must not claim idempotent
  primary-key drop coverage.

## Scope And Non-Goals

- Extend the existing `primary-key-ddl` selector rather than add another shard
  entry.
- Verify the already-open peer sees the initial `PRIMARY(id)` metadata.
- Verify plain duplicate `ALTER TABLE ... ADD PRIMARY KEY` returns MariaDB
  errno 1068.
- Verify `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS (code)` preserves the
  initial `PRIMARY(id)` metadata and duplicate-key enforcement.
- Keep the existing replacement to `PRIMARY(code)` and final reopen assertions.
- Do not add unsupported `DROP PRIMARY KEY IF EXISTS` coverage.
- Do not add crash injection, algorithm/lock matrices, descending/composite
  variants, or AUTO_INCREMENT variants in this slice.

## Design

The child ownerless process creates `app.ownerless_primary_key_base` with
`PRIMARY(id)` and signals the parent before any primary-key ALTER. The parent
verifies `information_schema.statistics` exposes `PRIMARY` over `id`, forced
primary-key reads work by `id`, a duplicate plain primary-key add returns errno
1068, and duplicate `id` inserts fail.

The child then runs `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS (code)` and
signals again. The parent verifies the no-op preserved `PRIMARY(id)` and that
duplicate `id` inserts still fail. The existing final phase then replaces the
clustered key with `PRIMARY(code)`, after which the parent verifies replacement
metadata, replacement-key duplicate rejection, old-key duplicate insertion, and
the existing ownerless/native reopen checks.

## Compatibility Impact

No new SQL surface is enabled. This expands ownerless evidence for MariaDB
primary-key DDL to include idempotent ADD-primary no-op preservation while
leaving unsupported idempotent primary-key drop outside the compatibility
claim.

## Directory And Lifecycle Impact

No directory layout changes. The selector continues to exercise native
MariaDB/InnoDB primary-key metadata inside the MyLite-owned database directory
and final state after forced `.shm` rebuild.

## Native Storage Impact

No storage format changes. The idempotent no-op branch must not mutate the
native clustered key; the existing replacement phase still performs the native
clustered-index rebuild.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `primary-key-ddl` selector.
- Run the matching embedded ownerless cross-process SQL CTest shard.
- Build and run the focused `primary-key-ddl` selector in
  `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Already-open ownerless peers see initial `PRIMARY(id)` metadata before the
  idempotent no-op.
- Plain duplicate primary-key add returns errno 1068.
- `ADD PRIMARY KEY IF NOT EXISTS` preserves the initial key definition and
  duplicate-key enforcement.
- The existing primary-key replacement to `PRIMARY(code)` still refreshes the
  peer and enforces the replacement key.
- Final rows and replacement-primary-key state survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- `DROP PRIMARY KEY IF EXISTS` is not a MariaDB grammar branch for this base and
  remains unsupported, not untested support.
- Crash-injected primary-key idempotent no-op, inline `CREATE TABLE`
  idempotency, algorithm/lock matrices, and randomized DDL oracle coverage
  remain planned broader work.
