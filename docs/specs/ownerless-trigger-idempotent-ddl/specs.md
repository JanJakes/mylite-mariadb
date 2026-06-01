# Ownerless Trigger Idempotent DDL

## Problem Statement

Ownerless trigger coverage proves create/fire/drop, replacement, UPDATE/DELETE
trigger bodies, ordering, and `SHOW CREATE TRIGGER`. It does not yet cover
MariaDB's idempotent trigger DDL paths, where duplicate trigger creation should
be a no-op with `IF NOT EXISTS` and missing trigger drops should be a no-op
with `IF EXISTS`.

Those paths matter for ownerless concurrency because trigger metadata is stored
in the table-level `.TRG` file and trigger-name `.TRN` files. A no-op trigger
DDL statement must not accidentally replace a trigger body, lose peer-visible
metadata, or leave an already-open ownerless peer with a stale trigger cache.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - The `CREATE ... TRIGGER` production routes through `trigger_tail`.
  - `trigger_tail` accepts `opt_if_not_exists` immediately before `sp_name`,
    so MariaDB supports `CREATE TRIGGER IF NOT EXISTS ...`.
  - `DROP TRIGGER` accepts `opt_if_exists` before the trigger name.
- `mariadb/sql/sql_trigger.cc`
  - `Table_triggers_list::create_trigger()` detects an existing trigger-name
    `.TRN` file. With `OR REPLACE`, it drops/replaces the old trigger. With
    `IF NOT EXISTS`, it pushes `ER_TRG_ALREADY_EXISTS` as a note and returns
    success without replacing the existing trigger body. Without either
    option, it returns `ER_TRG_ALREADY_EXISTS`.
  - `Table_triggers_list::drop_trigger()` removes the trigger from the table's
    trigger list, rewrites or removes the table-level `.TRG` file, and removes
    the trigger-name `.TRN` file.
  - `add_table_for_trigger_internal()` resolves `DROP TRIGGER` by reading the
    trigger-name `.TRN` file and treats a missing file as success when
    `IF EXISTS` is present.
- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_TRIGGER` and `SQLCOM_DROP_TRIGGER` dispatch through
    `mysql_create_or_drop_trigger()`.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats trigger `CREATE` and `DROP`
    statements as dictionary-generation boundaries, so already-open ownerless
    peers refresh around these statements.

## Design

Add a focused selector, `trigger-idempotent-ddl`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates InnoDB base/audit tables and creates an `AFTER INSERT`
   trigger with `CREATE TRIGGER IF NOT EXISTS`.
2. The parent observes `.TRG`/`.TRN` files and `INFORMATION_SCHEMA.TRIGGERS`,
   verifies plain duplicate `CREATE TRIGGER` returns MariaDB errno 1359, and
   fires the trigger through the already-open peer.
3. The child repeats `CREATE TRIGGER IF NOT EXISTS` for the same trigger name
   with a different body. The parent verifies the original body remains active
   through `SHOW CREATE TRIGGER` and audit-table effects.
4. The child runs `DROP TRIGGER IF EXISTS` for a missing trigger name. The
   parent verifies the existing trigger still exists and fires.
5. The child drops the real trigger with `DROP TRIGGER IF EXISTS`, then repeats
   the same drop as a no-op. The parent observes trigger metadata/file absence
   and verifies later DML no longer fires trigger bodies.
6. Final assertions verify base/audit rows plus trigger absence through
   ownerless and ordinary exclusive reopen before and after forced `.shm`
   rebuild.

## Scope

In scope:

- SQL-level ownerless coverage for `CREATE TRIGGER IF NOT EXISTS`.
- Plain duplicate trigger create rejection with MariaDB errno 1359.
- SQL-level ownerless coverage for `DROP TRIGGER IF EXISTS` over missing and
  existing trigger names.
- Already-open peer trigger metadata refresh and preservation of the original
  trigger body after a duplicate idempotent create.
- Trigger `.TRG` and `.TRN` file presence/absence inside the MyLite database
  directory.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- New production dictionary/storage code unless the selector exposes a bug.
- Trigger security/definer behavior, invalid trigger dependencies, stored
  functions called by triggers, and randomized trigger oracles.
- Crash/fault injection during trigger DDL.
- SQL-level table-lock fault injection.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for MariaDB idempotent trigger DDL while keeping broader trigger
semantics partial. Ordinary exclusive embedded behavior continues to inherit
MariaDB behavior.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises native trigger metadata
inside the MyLite-owned database directory:

- `datadir/app/ownerless_trigger_idempotent_base.TRG` while the trigger exists,
- `datadir/app/ownerless_trigger_idempotent_ai.TRN` while the trigger exists.

Final checks verify those files are absent after `DROP TRIGGER IF EXISTS` and
that the InnoDB base/audit tables survive ownerless/native reopen before and
after volatile shared-memory rebuild.

## Native Storage Impact

The base and audit tables are InnoDB. The selector exercises native INSERT
paths through trigger metadata refresh, but it does not change InnoDB file
formats, redo/page-version replay semantics, or page ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency,
public API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `trigger-idempotent-ddl` selector in `embedded-dev`.
- Run the adjacent trigger selector group in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the adjacent trigger selector group in `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest filters after implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees a trigger created by another process with
  `CREATE TRIGGER IF NOT EXISTS`.
- Plain duplicate `CREATE TRIGGER` returns MariaDB errno 1359.
- Duplicate `CREATE TRIGGER IF NOT EXISTS` does not replace the trigger body.
- `DROP TRIGGER IF EXISTS` for a missing trigger does not affect the existing
  trigger.
- Repeated `DROP TRIGGER IF EXISTS` for the real trigger removes it once and
  then succeeds as a no-op.
- Final trigger absence and base/audit table state survive ownerless/native
  reopen before and after forced `.shm` rebuild.
- Docs continue to mark untested trigger edge cases and crash recovery as
  planned.

## Risks And Open Questions

- This slice proves bounded no-op trigger DDL behavior. It does not prove crash
  recovery if a process dies while rewriting `.TRG`/`.TRN` files.
- Trigger security/definer behavior, invalid dependencies, trigger bodies that
  call stored functions, and randomized trigger oracles remain planned.
