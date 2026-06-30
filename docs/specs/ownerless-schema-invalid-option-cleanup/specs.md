# Ownerless Schema Invalid Option Cleanup

## Problem

Ownerless schema DDL recovery covers successful `db.opt` creation, rewrite,
comment, current-schema, and option-order paths. The remaining schema
file-lifecycle gap is failed schema-option validation: invalid charset or
collation clauses must not leave a rejected schema directory, rewrite the
existing schema `db.opt`, publish ownerless dictionary progress, or leave a
native file-operation checkpoint marker behind.

This slice adds deterministic ownerless SQL evidence for that failed-DDL
cleanup boundary without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5629-5640` parses schema options as repeated
  `create_database_option` entries covering default charset, default collation,
  and schema comment.
- `mariadb/sql/lex_charset.cc` validates charset/collation option
  consistency before those options are usable by schema DDL.
- `mariadb/sql/sql_db.cc:466-507` serializes schema defaults and comments into
  native `db.opt`.
- `mariadb/sql/sql_db.cc:896-918` implements `ALTER DATABASE` by locking the
  schema and rewriting `db.opt` through `write_db_opt()`.

## Scope And Non-Goals

In scope:

- invalid `CREATE DATABASE ... DEFAULT CHARACTER SET ...` cleanup;
- invalid named `ALTER DATABASE ... DEFAULT CHARACTER SET ...` cleanup;
- invalid named charset/collation pairing cleanup;
- invalid current-schema `ALTER DATABASE COMMENT ... DEFAULT CHARACTER SET ...`
  cleanup, proving the rejected comment does not persist;
- ownerless/native reopen and forced `.shm` rebuild checks.

Out of scope:

- crash injection inside MariaDB's failed `write_db_opt()` path;
- exhaustive invalid schema-option permutations;
- external randomized DDL/RQG stress.

## Design

Add a focused normal ownerless selector:

```sh
mylite_ownerless_cross_process_sql_test schema-invalid-options
```

The selector creates `ownerless_schema_invalid_options` with latin1 defaults
and an initial schema comment, then creates an InnoDB table that inherits those
defaults. It executes invalid schema-option statements that MariaDB rejects,
then verifies:

- the rejected schema was not created;
- the original schema still has the original charset, collation, and comment;
- the native `db.opt` file still exists under the MyLite datadir;
- the native file-operation checkpoint marker remains clear;
- a post-failure table still inherits the original latin1 defaults;
- ownerless/native reopen before and after forced `.shm` rebuild preserve the
  same metadata and rows.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The test records that
ownerless mode preserves MariaDB's failed schema-option behavior while keeping
durable schema metadata inside the MyLite database directory.

## Directory And Native Storage Impact

The covered durable file is `datadir/<schema>/db.opt`. Failed schema-option
DDL must not create durable files for rejected schemas or rewrite the existing
schema option file.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-schema-invalid-options$' --output-on-failure`.
- Run the adjacent schema ownerless selectors.
- Run the relevant production embedded schema selector.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Invalid schema-option DDL fails without durable schema side effects.
- The existing schema's `db.opt` metadata is preserved.
- The native file-operation checkpoint marker remains clear.
- Reopen and forced `.shm` rebuild preserve the same state.

## Risks And Follow-Up

- This is deterministic invalid-option cleanup evidence, not exhaustive schema
  grammar fuzzing.
- Long-running randomized external MariaDB/RQG DDL stress remains a completion
  follow-up.
