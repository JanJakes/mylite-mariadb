# SQL Prepare Command Size Profile

## Problem Statement

The aggressive MyLite minsize profile still keeps SQL-language prepared
statement commands:

- `PREPARE ... FROM ...`,
- `EXECUTE ... [USING ...]`,
- `EXECUTE IMMEDIATE ...`, and
- `DEALLOCATE PREPARE ...`.

These commands are distinct from the public `libmylite` prepared-statement API.
The public API uses MariaDB's embedded binary prepared-statement path to
preserve real bound-parameter semantics. SQL-language prepared statements are a
second dynamic-SQL surface that is less useful for an embedded API and keeps
extra `mysql_sql_stmt_*` roots in the linked runtime.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/sql_yacc.yy` parses the SQL commands through
  `LEX::stmt_prepare()`, `LEX::stmt_execute()`,
  `LEX::stmt_execute_immediate()`, and `LEX::stmt_deallocate_prepare()`.
- `vendor/mariadb/server/sql/sql_lex.cc` sets `SQLCOM_PREPARE`,
  `SQLCOM_EXECUTE`, `SQLCOM_EXECUTE_IMMEDIATE`, and
  `SQLCOM_DEALLOCATE_PREPARE` for those commands.
- `vendor/mariadb/server/sql/sql_parse.cc` dispatches those SQL commands to
  `mysql_sql_stmt_prepare()`, `mysql_sql_stmt_execute()`,
  `mysql_sql_stmt_execute_immediate()`, and `mysql_sql_stmt_close()`.
- `vendor/mariadb/server/sql/sql_prepare.cc` implements the SQL-language
  prepared-statement commands. The same file also implements the binary
  `COM_STMT_*` path used by the current public MyLite prepared API, so this
  slice must not remove the whole source file.
- `vendor/mariadb/server/mylite/mylite.cc` uses `mysql_stmt_prepare()`,
  `mysql_stmt_execute()`, `mysql_stmt_store_result()`, and related
  `mysql_stmt_*` functions for public `mylite_prepare()` and
  `mylite_step()`. Those functions dispatch `COM_STMT_*`, not SQL text
  `PREPARE` commands.

Pre-implementation linked symbol evidence from
`build/mariadb-minsize-no-client-fallbacks/mylite/mylite-open-close-smoke`
still includes `mysql_sql_stmt_prepare`, `mysql_sql_stmt_execute`,
`mysql_sql_stmt_execute_immediate`, and `mysql_sql_stmt_close` through
`mysql_execute_command()`.

## Scope

This slice may:

- add `MYLITE_DISABLE_SQL_PREPARE_COMMANDS`,
- enable it in `tools/build-mariadb-minsize.sh`,
- reject SQL-language prepared-statement commands with a stable unsupported
  diagnostic in the aggressive minsize profile,
- compile out the `mysql_sql_stmt_*` implementations when the profile is
  enabled, and
- add smoke coverage proving public MyLite prepared statements still work while
  SQL-language prepared statements fail explicitly.

## Non-Goals

This slice does not:

- remove public `mylite_prepare()`, `mylite_bind_*()`, `mylite_step()`,
  `mylite_reset()`, or `mylite_finalize()`,
- remove MariaDB binary `COM_STMT_*` handling,
- remove `Prepared_statement::prepare()` or server prepared-statement internals
  needed by public MyLite prepared statements,
- replace prepared statements with SQL string interpolation,
- change the public `libmylite` API, or
- change the `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_SQL_PREPARE_COMMANDS` as a `libmysqld` CMake option and
forward it as a compile definition.

When enabled, `mysql_execute_command()` should keep parsing the SQL syntax but
return `ER_NOT_SUPPORTED_YET` for:

- `SQLCOM_PREPARE`,
- `SQLCOM_EXECUTE`,
- `SQLCOM_EXECUTE_IMMEDIATE`, and
- `SQLCOM_DEALLOCATE_PREPARE`.

Use one helper message, `SQL PREPARE commands`, so all four related SQL command
forms report the same explicit feature boundary.

Wrap the SQL-language `mysql_sql_stmt_prepare()`,
`mysql_sql_stmt_execute()`, `mysql_sql_stmt_execute_immediate()`, and
`mysql_sql_stmt_close()` definitions in `sql_prepare.cc` behind
`#ifndef MYLITE_DISABLE_SQL_PREPARE_COMMANDS`. Leave `mysqld_stmt_prepare()`,
`mysqld_stmt_execute()`, `mysqld_stmt_fetch()`, `mysqld_stmt_reset()`,
`mysqld_stmt_close()`, and related binary-protocol prepared-statement helpers
untouched.

## Affected Subsystems

- SQL command dispatch in `sql_parse.cc`.
- SQL prepared-statement command implementation in `sql_prepare.cc`.
- Aggressive minsize build configuration.
- `libmylite` open/close smoke coverage.
- Production size analysis.

## DDL Metadata Routing Impact

No DDL metadata routing should change. Ordinary `CREATE`, `ALTER`, `DROP`,
and `RENAME` paths stay in MariaDB SQL execution and MyLite storage handlers.

## Single-File And Embedded-Lifecycle Impact

No file ownership, catalog, locking, recovery, or runtime lifecycle behavior
changes. The slice removes a SQL dynamic-statement surface while retaining the
embedded prepared-statement path used by public `libmylite` handles.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

Compatibility impact: applications that issue SQL text `PREPARE`, `EXECUTE`,
`EXECUTE IMMEDIATE`, or `DEALLOCATE PREPARE` through `mylite_exec()` lose that
SQL surface in the aggressive profile. Applications should use the public
`libmylite` prepared-statement API instead.

## Binary-Size Impact

Expected savings are likely modest. `sql_prepare.cc` remains linked because the
public MyLite prepared API still needs MariaDB's binary prepared-statement
internals. The potential win is limited to SQL-language command helpers,
dynamic SQL string handling, and the references from `mysql_execute_command()`.

Implemented measurements against the preceding
`embedded-client-fallback-size-profile` baseline:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,504,414 | 25,493,040 | -11,374 |
| `mylite/libmylite.a` | 122,800 | 122,792 | -8 |
| `storage/mylite/libmylite_embedded.a` | 388,456 | 388,456 | 0 |
| unstripped `mylite-open-close-smoke` | 6,585,232 | 6,582,456 | -2,776 |
| stripped `mylite-open-close-smoke` | 4,628,736 | 4,626,880 | -1,856 |

`llvm-size` total for the linked open-close smoke changed from 4,852,372 to
4,848,276 bytes (-4,096). `sql_prepare.cc.o` changed from 33,430 to 30,365
bytes (-3,065). The linked smoke no longer contains
`mysql_sql_stmt_prepare`, `mysql_sql_stmt_execute`,
`mysql_sql_stmt_execute_immediate`, or `mysql_sql_stmt_close`, while public
prepared-statement roots such as `mylite_prepare`, `mylite_step`,
`mysql_stmt_prepare`, and `mysql_stmt_execute` remain.

Measure against `embedded-client-fallback-size-profile`:

- `libmysqld/libmariadbd.a`,
- unstripped `mylite-open-close-smoke`,
- stripped `mylite-open-close-smoke`,
- `llvm-size` totals, and
- presence or absence of `mysql_sql_stmt_prepare`,
  `mysql_sql_stmt_execute`, `mysql_sql_stmt_execute_immediate`, and
  `mysql_sql_stmt_close` in the linked smoke.

## License, Trademark, And Dependency Impact

No new dependency or license impact. This is a GPL-2.0-only MariaDB-derived
build-profile change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

Additional checks:

- public MyLite prepared-statement smoke sections still pass,
- SQL text `PREPARE`, `EXECUTE`, `EXECUTE IMMEDIATE`, and
  `DEALLOCATE PREPARE` fail with `ER_NOT_SUPPORTED_YET`, and
- linked symbol checks confirm the SQL-language `mysql_sql_stmt_*` entry
  points are absent.

## Acceptance Criteria

- Passed: the minsize build succeeds with
  `MYLITE_DISABLE_SQL_PREPARE_COMMANDS=ON`.
- Passed: public `libmylite` prepared statements and parameter binding still
  pass.
- Passed: SQL-language prepared-statement commands fail explicitly.
- Passed: current embedded bootstrap, open/close, storage-engine, and
  compatibility harness checks pass.
- Passed: size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-prepare \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The open-close report includes:

```text
exec_sql_prepare_messages=prepare:This version of MariaDB doesn't yet support 'SQL PREPARE commands'|execute:This version of MariaDB doesn't yet support 'SQL PREPARE commands'|execute_immediate:This version of MariaDB doesn't yet support 'SQL PREPARE commands'|deallocate:This version of MariaDB doesn't yet support 'SQL PREPARE commands'
prepared_rows=1:one:610062,2:NULL:NULL
prepared_bound_rows=1:-1234567890123:9223372036854775810:3.500000:hello:610062,2:NULL:7:-2.250000:NULL:NULL,3:0:0:0.250000:custom:7a
prepared_bind_destructor_count=1
```

There was one discarded verification run where storage and embedded smokes were
started concurrently against the same build directory and CMake reported
generated-symlink races. Both affected smokes were rerun sequentially and
passed.

## Risks And Unresolved Questions

- This is a SQL compatibility loss. It should remain an aggressive profile
  decision unless product compatibility later allows it by default.
- Archive savings may be small because `sql_prepare.cc` still contains the
  binary prepared-statement implementation needed by public MyLite prepared
  statements.
- Parser-side `LEX` helper code for SQL prepared statements may remain because
  the syntax is still parsed before execution is rejected.
