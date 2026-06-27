# Ownerless Current Schema Comment Live Recovery

## Problem

Ownerless schema-comment crash recovery covered named
`ALTER DATABASE <schema> COMMENT = 'literal'`. MariaDB also accepts the
current-schema form `ALTER DATABASE COMMENT = 'literal'`, which resolves the
target schema from the session default database. Before this slice, MyLite's
ownerless schema-option classifier rejected that grammar form because
`COMMENT` was not accepted where a schema identifier was expected.

The current-schema form should recover the completed native `db.opt` rewrite
while a peer remains live, preserve schema defaults, and keep the native
file-operation marker clear like the named schema-comment path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` has a distinct
  `ALTER DATABASE COMMENT_SYM opt_equal TEXT_STRING_sys` production. It sets
  `SQLCOM_ALTER_DB`, leaves `lex->name` empty, and then copies the session
  current database into the statement name.
- `mariadb/sql/sql_db.cc` `mysql_alter_db_internal()` rewrites the schema
  option file through the same `write_db_opt()` path used by named
  `ALTER DATABASE`.
- `packages/libmylite/src/database.cc` already accepts quoted schema-comment
  options after a named schema identifier. The missing piece was the
  current-schema statement shape.

## Design

Extend `ownerless_alter_schema_recovery_statement()` to accept
`ALTER DATABASE COMMENT ...` by passing the token stream directly to the
existing schema-option consumer when token 2 is `COMMENT`. The current-schema
shortcut is limited to `DATABASE`, not `SCHEMA`, matching MariaDB's grammar,
and it still fails closed for missing, unquoted, or unrelated trailing tokens.

Add hook-build selector `dictionary-current-schema-comment-crash`:

- create `ownerless_schema_current_comment_crash` with `latin1` defaults and an
  initial comment,
- create and write an InnoDB table whose string column inherits `latin1`,
- crash a writer after `USE ownerless_schema_current_comment_crash; ALTER
  DATABASE COMMENT = 'ownerless current schema comment'` completes natively but
  before ownerless dictionary finish,
- verify live-peer recovery sees the updated `SCHEMA_COMMENT`, preserved
  defaults, and clear native file-operation marker, and
- verify ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Current-schema `ALTER DATABASE COMMENT = 'literal'` recovery at
  `dictionary-before-finish`.
- Quoted string comments using the already-supported literal policy.
- Native `db.opt` presence, live-peer recovery, and reopen checks.

Out of scope:

- Current-schema default charset/collation-only forms.
- Character-set introducers on schema comment literals.
- Invalid or overlong comment cleanup.
- Exhaustive schema option ordering and randomized external DDL stress.

## Compatibility Impact

No SQL feature is invented. The slice aligns ownerless recovery classification
with MariaDB's accepted current-schema schema-comment form.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises MariaDB's native
`datadir/<schema>/db.opt` under the MyLite database directory, live ownerless
recovery, no-live close, forced `.shm` rebuild, and ordinary native exclusive
reopen.

## Native Storage Impact

Schema comments are SQL-layer metadata. The test creates InnoDB tables before
and after recovery to prove inherited defaults and native table storage remain
usable.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The slice adds a focused classifier branch, hook test coverage, CTest
registration, and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the direct selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-current-schema-comment-crash`.
- Run the focused CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-(schema-comment|current-schema-comment)-crash$' --output-on-failure`.
- Run adjacent schema crash selectors, production compile, format check, and
  diff check.

## Acceptance Criteria

- The focused selector reaches the dictionary-finish fault and exits cleanly.
- Live-peer recovery observes the current-schema comment rewrite while
  preserving schema defaults and keeping the native file-operation marker
  clear.
- The schema directory and `db.opt` stay inside the MyLite database directory.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- Current-schema charset/collation option forms remain unclaimed until a
  separate slice proves that parser shape and recovery behavior.
- Broader schema option permutations and external randomized DDL stress remain
  planned.
