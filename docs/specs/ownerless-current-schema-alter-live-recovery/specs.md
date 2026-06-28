# Ownerless Current Schema Alter Live Recovery

## Problem Statement

Ownerless schema-default crash recovery covered named
`ALTER DATABASE <schema> DEFAULT CHARACTER SET ... COLLATE ...` and the
`ALTER SCHEMA <schema>` synonym. MariaDB also accepts the current-schema form:

```sql
USE app;
ALTER DATABASE DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
```

Before this slice, MyLite's ownerless recovery classifier treated the token
after `ALTER DATABASE` as a required schema name except for the current-schema
`COMMENT` form. A writer killed after MariaDB rewrote the native `db.opt` file
for the current-schema default-option form could therefore miss focused
live-peer recovery classification.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:7471-7485` parses
  `ALTER DATABASE ident_or_empty create_database_options`, sets
  `SQLCOM_ALTER_DB`, and copies the current database into `lex->name` when no
  explicit schema identifier is present.
- `mariadb/mysql-test/main/ctype_create.test:96-101` and
  `mariadb/mysql-test/main/ctype_create.result:68-73` prove the accepted
  `USE db; ALTER DATABASE DEFAULT CHARACTER SET ...` form and the `1046`
  error when no current database is selected.
- `mariadb/sql/sql_db.cc` `mysql_alter_db_internal()` rewrites
  `MY_DB_OPT_FILE` (`db.opt`) for altered schema defaults.
- `packages/libmylite/src/database.cc` uses token-level ownerless dictionary
  recovery classification before arming the existing unsafe
  `dictionary-before-finish` test hook.

## Design

Extend `ownerless_alter_schema_recovery_statement()` so `ALTER DATABASE`
without an explicit schema name can start directly with schema option tokens:

- `COMMENT`,
- `DEFAULT`,
- `CHARACTER`,
- `CHARSET`,
- `COLLATE`.

Those tokens are passed to the existing bounded schema-option consumer. The
shortcut is still limited to `ALTER DATABASE`; `ALTER SCHEMA` remains on the
named-schema path. Quoted schema names that happen to spell option words keep
using the existing identifier path.

Add hook-build selector `dictionary-current-schema-alter-crash`:

- create `ownerless_schema_current_alter_crash` with `latin1` defaults,
- create a pre-alter InnoDB table whose `VARCHAR` column inherits `latin1`,
- kill a writer after `USE ownerless_schema_current_alter_crash; ALTER
  DATABASE DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci` completes
  natively but before ownerless dictionary finish,
- verify live-peer recovery with the native file-operation marker clear,
- verify recovered schema defaults, pre-alter table metadata preservation, and
  post-recovery table default inheritance, and
- verify ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Current-schema `ALTER DATABASE DEFAULT CHARACTER SET/COLLATE` recovery at
  `dictionary-before-finish`.
- Native `datadir/<schema>/db.opt` rewrite evidence.
- Live-peer recovery, marker-clear behavior, ownerless/native reopen, and
  forced shared-memory rebuild.

Out of scope:

- `ALTER SCHEMA` current-schema grammar, which MariaDB does not expose in the
  same form.
- Invalid charset/collation cleanup, default value keyword permutations,
  character-set introducers, and exhaustive option ordering.
- SQL-level table-lock fault injection.
- External MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice aligns ownerless crash recovery
classification with a MariaDB-accepted `ALTER DATABASE` form that already
rewrites schema defaults through native MariaDB code.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or durable format changes. The test exercises MariaDB's
native schema option file under the MyLite-owned `datadir/`, ownerless
dictionary live recovery, no-live cleanup, ownerless/native reopen, and forced
`.shm` rebuild.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds one bounded classifier branch, one hook selector, CTest
registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-current-schema-alter-crash`.
- Run adjacent schema crash selectors through CTest.
- Run production format and CI-production-build guards.
- Build the PHP embedded production ownerless test binary.
- Run `git diff --check`.

## Acceptance Criteria

- The focused selector reaches `dictionary-before-finish` and kills the writer
  after native ALTER success.
- Live-peer recovery observes `utf8mb4` / `utf8mb4_unicode_ci` schema defaults
  while the native file-operation marker remains clear.
- The pre-alter table keeps `latin1` column metadata.
- A post-recovery table inherits the recovered `utf8mb4` defaults.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  observe the same schema, table metadata, and rows.

## Risks And Follow-Up

- This covers a focused current-schema default-option boundary, not every
  schema-option ordering or invalid-option cleanup path.
- Broader schema DDL option matrices and external randomized DDL stress remain
  planned.
