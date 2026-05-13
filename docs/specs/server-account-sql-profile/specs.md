# Server Account SQL Profile

## Problem Statement

MyLite opens an application-owned local `.mylite` file in-process. It does not
expose a daemon login model, network users, grant tables, roles, or
administrator-owned server accounts in the aggressive embedded profile.

The current embedded no-access-check path inherits MariaDB stubs that can make
server account statements such as `CREATE USER`, `GRANT`, `REVOKE`, and
`SHOW GRANTS` report success. That is not acceptable for MyLite: unsupported
server administration SQL must fail explicitly rather than silently pretending
to work.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/include/my_global.h` defines
  `NO_EMBEDDED_ACCESS_CHECKS` for embedded builds unless
  `HAVE_EMBEDDED_PRIVILEGE_CONTROL` is configured.
- `vendor/mariadb/server/cmake/build_configurations/mysql_release.cmake` sets
  `HAVE_EMBEDDED_PRIVILEGE_CONTROL` for Debian-style release builds, so MyLite
  must not rely on one fixed upstream default.
- `vendor/mariadb/server/sql/sql_parse.cc` keeps `CREATE USER`, `DROP USER`,
  `ALTER USER`, role administration, `SHOW CREATE USER`, and `SHOW GRANTS`
  cases under `#ifndef NO_EMBEDDED_ACCESS_CHECKS`.
- `vendor/mariadb/server/sql/sql_parse.cc` dispatches `GRANT` and `REVOKE`
  through `lex->m_sql_cmd->execute(thd)`.
- `vendor/mariadb/server/sql/sql_acl.cc` defines no-access-check
  `Sql_cmd_grant_*::execute()` stubs that currently call `my_ok(thd)`.
- `vendor/mariadb/server/mylite/mylite.cc` starts the embedded runtime with
  `--skip-grant-tables`, because MyLite does not yet own a server account
  catalog.

## Scope

This slice may:

- add a MyLite minsize build option for server account SQL rejection,
- reject user, role, grant, revoke, `SHOW GRANTS`, and `SHOW CREATE USER`
  statements with `ER_NOT_SUPPORTED_YET`,
- keep ordinary DML, DDL, metadata, and local MyLite file opening unaffected,
- add smoke coverage proving unsupported account SQL fails explicitly, and
- record size impact in the production size analysis.

## Non-Goals

This slice does not:

- remove ordinary MariaDB SQL semantics such as table DDL, query execution,
  scalar functions, or `SHOW` statements unrelated to server accounts,
- design a MyLite account/grant catalog,
- claim SQL authorization support in the current embedded profile,
- change the public `libmylite` API, or
- change non-MyLite MariaDB embedded builds.

## Proposed Design

Add `MYLITE_DISABLE_SERVER_ACCOUNT_SQL` as an off-by-default
`libmysqld/CMakeLists.txt` option. Enable it only in
`tools/build-mariadb-minsize.sh`.

When enabled:

- `sql_parse.cc` should reject account-management SQL cases that are otherwise
  compiled out under `NO_EMBEDDED_ACCESS_CHECKS`.
- `sql_acl.cc` should make no-access-check `GRANT` / `REVOKE` command stubs
  return an error instead of `my_ok(thd)`.
- Diagnostics should use MariaDB's ordinary `ER_NOT_SUPPORTED_YET` and SQLSTATE
  `42000`, with a message naming server users and grants in the MyLite minsize
  profile.

## Affected Subsystems

- SQL command dispatch in `sql_parse.cc`.
- Account/grant command execution in `sql_acl.cc`.
- MyLite minsize build configuration.
- `libmylite` open/close smoke coverage.

## Single-File And Embedded-Lifecycle Impact

No storage or file-format behavior changes. The slice prevents account/grant
statements from mutating inherited grant tables or falsely reporting success
while MyLite runs without a server account catalog.

## Public API And File-Format Impact

No public API or file-format change. The observable effect is a more honest SQL
diagnostic for an unsupported server-administration surface.

## Binary-Size Impact

This is primarily a correctness gate for an already embedded-only surface.
Because the current aggressive profile already compiles `sql_acl.cc` through
`NO_EMBEDDED_ACCESS_CHECKS`, the expected size change is small. Measure the
static archive, stripped linked smoke, and minimal linked consumer after
implementation.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-embedded-bootstrap-smoke.sh \
  tools/run-storage-engine-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The open/close smoke should cover:

- `CREATE USER`,
- `DROP USER`,
- `GRANT`,
- `REVOKE`, and
- `SHOW GRANTS`.

## Acceptance Criteria

- Passed: server account SQL in the minsize profile returns `MYLITE_ERROR`.
- Passed: MariaDB diagnostics report `ER_NOT_SUPPORTED_YET` and SQLSTATE
  `42000`.
- Passed: messages identify server users and grants in the MyLite minsize
  profile.
- Passed: existing lifecycle, storage, and compatibility smokes still pass.
- Passed: size results are recorded in this spec and in
  `docs/research/production-size-analysis.md`.

## Verification

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-server-account-sql \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-embedded-bootstrap-smoke.sh \
  tools/run-storage-engine-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The first parallel attempt to run embedded bootstrap and storage smokes in the
same build directory hit CMake symlink creation races. Sequential reruns passed.

Measured against `build/mariadb-minsize-no-sql-exceptions`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 22,437,126 | 22,437,702 | +576 |
| `mylite/libmylite.a` | 76,696 | 76,696 | 0 |
| unstripped `mylite-open-close-smoke` | 5,858,840 | 5,860,688 | +1,848 |
| stripped `mylite-open-close-smoke` | 3,993,848 | 3,995,560 | +1,712 |
| unstripped minimal executable probe | 5,734,200 | 5,734,328 | +128 |
| stripped minimal executable probe | 3,886,072 | 3,886,264 | +192 |
| unstripped shared-object probe | 5,733,936 | 5,734,144 | +208 |
| stripped shared-object probe | 3,886,048 | 3,886,256 | +208 |

## Risks And Unresolved Questions

- A future MyLite authorization design may need a real account/grant catalog.
  This slice does not close that path; it only prevents current false success.
- This option belongs to the aggressive embedded profile until the product
  decides whether any account SQL should exist in the default embedded build.
