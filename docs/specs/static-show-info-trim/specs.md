# Static SHOW Info Trim

## Problem Statement

The embedded archive still builds MariaDB's static server-information result
producers for `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES`.
These commands expose project attribution, contributor, and generic server
privilege metadata. They do not inspect application schemas, rows, diagnostics,
native storage, or MyLite database-directory state.

This slice removes only those static information producers from the default
embedded archive.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
  `SHOW PRIVILEGES` into `SQLCOM_SHOW_AUTHORS`,
  `SQLCOM_SHOW_CONTRIBUTORS`, and `SQLCOM_SHOW_PRIVILEGES`.
- `mariadb/sql/sql_parse.cc` dispatches those commands to
  `mysqld_show_authors()`, `mysqld_show_contributors()`, and
  `mysqld_show_privileges()`.
- `mariadb/sql/sql_show.cc` implements those result producers.
- `SHOW AUTHORS` and `SHOW CONTRIBUTORS` read static rows from
  `mariadb/sql/authors.h` and `mariadb/sql/contributors.h`.
- `SHOW PRIVILEGES` reads the static `sys_privileges[]` table in
  `mariadb/sql/sql_show.cc`.
- Ordinary `SHOW VARIABLES`, `SHOW WARNINGS`, and table/schema metadata
  inspection use separate code paths.

## Proposed Design

Add `MYLITE_WITH_STATIC_SHOW_INFO`, defaulting to `ON` for normal MariaDB
builds. The MyLite embedded baseline sets it to `OFF`.

When disabled, `mariadb/sql/sql_show.cc` stops including `authors.h` and
`contributors.h`, and compiles out the three static result producers plus the
`SHOW PRIVILEGES` static table. `mariadb/sql/sql_parse.cc` keeps the command
enums and parser dispatch but reports an unsupported feature if the public
MyLite policy is bypassed.

The public SQL policy rejects direct and prepared `SHOW AUTHORS`, `SHOW
CONTRIBUTORS`, and `SHOW PRIVILEGES` with the standard server-owned surface
diagnostic. Supported `SHOW` surfaces remain available.

## Affected MariaDB Subsystems

- Embedded CMake profile options and definitions.
- Static `SHOW AUTHORS` / `SHOW CONTRIBUTORS` / `SHOW PRIVILEGES` producers in
  `sql_show.cc`.
- Fail-closed dispatch in `sql_parse.cc`.
- Public MyLite SQL policy and server-surface tests.

## Compatibility Impact

The three static server-information commands become explicitly unsupported in
the default embedded profile. This removes server attribution and privilege
help output, not application table metadata, diagnostics, SQL execution, native
storage, or public C API behavior.

`SHOW PRIVILEGES` is account/server metadata. MyLite already treats server
accounts, grants, roles, and password administration as outside the embedded
core API, so this remains aligned with the existing server-surface policy.

## Database-Directory And Lifecycle Impact

None. No runtime files, native storage files, locks, or temporary paths change.

## Public API Impact

No C API symbols or structs change. Direct and prepared SQL entry points return
the existing `MYLITE_ERROR` policy diagnostic for the three static `SHOW`
commands.

## Native Storage Impact

None. InnoDB, MyISAM, Aria, MEMORY, and supported metadata inspection paths are
unchanged.

## Binary-Size Impact

Measured on 2026-05-20 with `tools/mariadb-embedded-build all`, omitting
static `SHOW` information reduces the stripped embedded archive to 27,137,632
bytes / 25.88 MiB with 705 members. That is 32,936 bytes smaller than the
previous 27,170,568-byte system-variable help-text baseline, with no
member-count change.

## License Or Dependency Impact

No new dependencies or license changes.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev -L compat.server-surface --output-on-failure`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The default embedded baseline sets `MYLITE_WITH_STATIC_SHOW_INFO=OFF`.
- Normal MariaDB builds keep the option defaulted to `ON`.
- Direct and prepared static `SHOW` information SQL is rejected through MyLite
  policy.
- The MariaDB dispatch path fails closed if the public policy is bypassed.
- Ordinary `SHOW VARIABLES` remains available.
- Current archive size and member count are recorded.

## Risks And Unresolved Questions

- `SHOW PRIVILEGES` is more useful than `SHOW AUTHORS` and
  `SHOW CONTRIBUTORS`, but it is still server privilege metadata for an
  account/grant model MyLite does not expose in the core embedded API.
- Parser support remains so upstream syntax and command enums stay intact.
