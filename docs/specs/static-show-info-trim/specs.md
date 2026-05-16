# Static SHOW Info Trim

## Problem

The default embedded profile still builds static MariaDB server-information
result producers for `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
`SHOW PRIVILEGES`. These commands expose project attribution, sponsor, and
server privilege metadata. They do not inspect application schemas, rows,
diagnostics, or MyLite file-owned state.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB documents `SHOW AUTHORS` as displaying people who work on MariaDB:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-authors>.
- MariaDB documents `SHOW CONTRIBUTORS` as displaying organizations and people
  who financially contribute to MariaDB:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-contributors>.
- MariaDB documents `SHOW PRIVILEGES` as listing the system privileges the
  server supports:
  <https://mariadb.com/kb/en/show-privileges/>.
- `mariadb/sql/sql_yacc.yy` parses the commands into
  `SQLCOM_SHOW_AUTHORS`, `SQLCOM_SHOW_CONTRIBUTORS`, and
  `SQLCOM_SHOW_PRIVILEGES`.
- `mariadb/sql/sql_parse.cc` dispatches those commands to
  `mysqld_show_authors()`, `mysqld_show_contributors()`, and
  `mysqld_show_privileges()`.
- `mariadb/sql/sql_show.cc` implements those result producers. `SHOW AUTHORS`
  and `SHOW CONTRIBUTORS` pull static rows from `authors.h` and
  `contributors.h`; `SHOW PRIVILEGES` emits a static privilege list.

## Design

- Add `MYLITE_WITH_STATIC_SHOW_INFO`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When disabled, compile out the static result producers and static
  authors/contributors includes from `sql_show.cc`.
- Add a fail-closed dispatch branch in `sql_parse.cc` in case these statements
  reach MariaDB execution outside the public MyLite C API policy.
- Reject direct and prepared `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
  `SHOW PRIVILEGES` through the public MyLite SQL policy with stable
  unsupported-surface diagnostics.
- Keep ordinary application-facing `SHOW` surfaces that MyLite currently
  supports, such as `SHOW VARIABLES`, `SHOW WARNINGS`, and table/schema
  metadata inspection.

## Compatibility Impact

The three static server-information commands become explicitly unsupported.
This removes server self-description output, not application table metadata,
warning diagnostics, SQL execution, or storage-engine routing.

## Single-File And Embedded Lifecycle Impact

No file-format change. The trim does not add sidecars, durable metadata,
runtime companions, or startup/shutdown state.

## Storage-Engine Routing Impact

No storage-routing change. Static SHOW information does not touch handler
registration, table routing, row storage, indexes, or the MyLite catalog.

## Public API Impact

No C API surface change. `mylite_exec()` and `mylite_prepare()` return stable
unsupported-surface diagnostics for the three static SHOW commands.

## Binary-Size Impact

The measured size impact is small and comes from omitted static row arrays and
result construction paths. The default embedded archive is 37,160 bytes smaller
with the same member count, and the storage-smoke archive is also 37,160 bytes
smaller with the same member count.

## License And Dependency Impact

No new dependency and no license change. The slice only compiles out retained
MariaDB GPL-2.0 source in the embedded profile.

## Test And Verification Plan

- Add direct SQL tests for rejected `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
  `SHOW PRIVILEGES`.
- Add prepared-statement rejection tests for representative static SHOW info
  commands.
- Add ordinary `SHOW VARIABLES` / warning-path smoke to prove supported SHOW
  commands remain available.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Verify `MYLITE_WITH_STATIC_SHOW_INFO:BOOL=OFF` appears in measured cache
  options.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` CMake build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_STATIC_SHOW_INFO=OFF`.
- Direct and prepared static SHOW info SQL is rejected with MyLite diagnostics.
- Supported SHOW surfaces used by existing tests remain available.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- `SHOW PRIVILEGES` is more generally useful than `SHOW AUTHORS` and
  `SHOW CONTRIBUTORS`, but it is still server privilege metadata for an
  account/grant model MyLite does not expose in the core embedded API.
- Parser support remains so upstream syntax and command enums stay intact; the
  unsupported behavior must be enforced before users can rely on static server
  metadata output.
