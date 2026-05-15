# SQL HELP Command Trim

## Problem

The default MyLite embedded profile still builds MariaDB's SQL `HELP` command.
`HELP` is an interactive server/client convenience command backed by server
help tables in the `mysql` schema. MyLite's core embedded API should not depend
on install-time server help tables, and application runtimes do not need
in-database command help in the linked library.

This slice removes the full `HELP` implementation from the default embedded
archive while preserving an explicit unsupported-command diagnostic.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documents `HELP search_string` as a command that displays
  information from server help tables and requires initialized help metadata:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/help-command>.
- `mariadb/sql/sql_yacc.yy:2234` parses `HELP_SYM ident_or_text`,
  sets `SQLCOM_HELP`, and stores the argument in `LEX::help_arg`.
- `mariadb/sql/sql_parse.cc:65` includes `sql_help.h`, and
  `mariadb/sql/sql_parse.cc:4045` executes `SQLCOM_HELP` by calling
  `mysqld_help(thd, lex->help_arg)`.
- `mariadb/sql/sql_prepare.cc:107` includes `sql_help.h`,
  `mariadb/sql/sql_prepare.cc:2132` prepares `HELP` metadata through
  `mysqld_help_prepare()`, and `mariadb/sql/sql_prepare.cc:2415` routes
  `SQLCOM_HELP` prepared statements into that path.
- `mariadb/sql/sql_help.h:26` exposes only `mysqld_help()` and
  `mariadb/sql/sql_help.h:28` exposes `mysqld_help_prepare()`.
- `mariadb/sql/sql_help.cc:33` defines the used fields for `help_topic`,
  `help_category`, `help_keyword`, and `help_relation`;
  `mariadb/sql/sql_help.cc:749` initializes those four `mysql` schema tables;
  `mariadb/sql/sql_help.cc:829` prepares metadata; and
  `mariadb/sql/sql_help.cc:1111` executes the direct command.
- The embedded source list in `mariadb/libmysqld/CMakeLists.txt` previously
  linked `../sql/sql_help.cc`. The normal `mariadb/sql/CMakeLists.txt:185`
  server SQL target also links `sql_help.cc`, but this slice only changes the
  embedded profile.

## Design

- Add `MYLITE_WITH_HELP_COMMAND`, defaulting to `ON` for upstream-compatible
  build behavior.
- Set `MYLITE_WITH_HELP_COMMAND=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When disabled in `mariadb/libmysqld/CMakeLists.txt`, replace
  `../sql/sql_help.cc` in `SQL_EMBEDDED_SOURCES` with
  `mylite_help_command_stub.cc`.
- Keep parser syntax, `SQLCOM_HELP`, and `sql_parse.cc` / `sql_prepare.cc`
  routing intact. The replacement object implements `mysqld_help()` and
  `mysqld_help_prepare()` with `ER_NOT_SUPPORTED_YET`.
- Add a `libmylite` SQL-policy rejection for top-level `HELP` statements so
  public direct and prepared APIs fail before MariaDB execution with stable
  MyLite diagnostics.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- `libmylite` unsupported SQL policy.
- Embedded direct and prepared SQL diagnostics tests.
- Compatibility harness server-surface documentation.
- Size-profile documentation and roadmap status.

## MySQL/MariaDB Compatibility Impact

`HELP` moves to explicit out-of-scope status for the default MyLite embedded
profile. This is a deliberate embedded-runtime tradeoff: MyLite preserves
application SQL execution surfaces, while omitting server-installation help
metadata that is normally populated outside application data files.

The normal MariaDB SQL target keeps the full `HELP` implementation, so this
slice does not prevent building an upstream-style server target for comparison.

## DDL Metadata Routing Impact

None. `HELP` reads MariaDB server help metadata tables but does not publish
user table definitions, row data, indexes, constraints, or MyLite catalog
records.

## Single-File And Embedded-Lifecycle Impact

Removing `sql_help.cc` avoids opening `mysql.help_*` tables from the embedded
runtime. The stub performs no file access, table open, transaction setup, or
catalog mutation.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. The public behavior is a stable
`MYLITE_ERROR`, SQLSTATE `HY000`, MariaDB errno `0`, and message containing
`HELP SQL command` when a direct or prepared public API receives top-level
`HELP`.

## Storage-Engine Routing Impact

None. The command does not route through table storage engines.

## Wire-Protocol Or Integration-Package Impact

Core `libmylite` rejects `HELP`. A future wire-protocol wrapper can translate
client-side help requests outside the embedded library if a CLI requires them.

## Binary-Size Impact

The full `sql_help.cc` object contains server help-table setup, lookup, and
result-shaping code. The implemented trim removes 53,000 bytes from both
measured embedded archives and reduces linked smoke binaries where the linker
previously retained help-command code.

## License And Dependency Impact

No new dependency. The replacement stub is GPL-2.0-compatible first-party
MyLite code inside the GPL-2.0 MariaDB-derived tree.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm `libmariadbd.a` contains `mylite_help_command_stub.cc.o` and no
  `sql_help.cc.o`.
- Confirm full `sql_help.cc` helper symbols are absent from default and
  storage-smoke archives.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Build the normal MariaDB `sql` target to confirm the non-embedded server path
  still compiles with full `HELP`.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- The default embedded cache records `MYLITE_WITH_HELP_COMMAND=OFF`.
- Direct `HELP 'contents'`, lowercase `help contents`, executable-comment
  `HELP`, and prepared `HELP 'contents'` are rejected by `libmylite` before
  MariaDB execution.
- Quoted text containing `HELP` remains accepted.
- The embedded archive links the MyLite stub instead of full `sql_help.cc`.
- Documentation and compatibility matrix mark SQL `HELP` explicitly
  unsupported for the default embedded profile.
- Size measurements are recorded.

## Risks And Unresolved Questions

- Parser syntax remains present, so internal callers that bypass `libmylite`
  can still parse `HELP` and receive the stub diagnostic. That keeps the patch
  narrow and avoids parser churn, but means syntax-level omission is not proven.
- Future CLI packages may want user-facing help behavior. That should be
  implemented as client-side documentation or wrapper behavior, not by linking
  server help-table scans into the core library.

## Implementation Results

Implemented with `MYLITE_WITH_HELP_COMMAND=OFF` in the default embedded
profile. `mariadb/libmysqld/CMakeLists.txt` now links
`mylite_help_command_stub.cc` in place of `../sql/sql_help.cc` for
`sql_embedded`; the normal `sql` target still builds `sql_help.cc`.

Verification:

```sh
tools/mariadb-embedded-build configure
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset storage-smoke-dev
ctest --preset storage-smoke-dev --output-on-failure
tools/mylite-compat-harness report server-surface
tools/mylite-size-report
cmake --build build/mariadb-embedded --target sql
```

Observed archive contents:

- both default and storage-smoke archives include
  `mylite_help_command_stub.cc.o`;
- neither archive includes `sql_help.cc.o`;
- full helper symbols such as `initialize_tables_for_help_command()` and
  `search_topics()` are absent from both archives;
- `mysqld_help()` and `mysqld_help_prepare()` remain as stub entry points.

Measured size impact compared with the previous no-exceptions baseline:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| Default `libmariadbd.a` | 28,200,816 | -53,000 |
| Storage-smoke `libmariadbd.a` | 28,381,400 | -53,000 |
| Embedded open-close smoke | 17,962,064 | -18,512 |
| Embedded exec smoke | 17,979,800 | -18,416 |
| Embedded statement smoke | 17,994,816 | -2,000 |
| Embedded warning smoke | 17,961,696 | -18,512 |
| Embedded comparison smoke | 18,068,400 | -18,544 |
| Storage-smoke open-close smoke | 18,040,672 | -18,512 |
| Storage-smoke exec smoke | 18,074,920 | -1,936 |
| Storage-smoke statement smoke | 18,073,424 | -18,512 |
| Storage-smoke warning smoke | 18,056,816 | -2,000 |
| Storage-smoke comparison smoke | 18,142,512 | -18,544 |
| Storage-engine smoke | 18,309,360 | -18,512 |
