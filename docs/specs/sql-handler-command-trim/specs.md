# SQL HANDLER Command Trim

## Problem

The default embedded profile still carries MariaDB's SQL `HANDLER` command
runtime. `HANDLER` is a direct table-handler cursor surface that opens a table
against the storage-engine interface and then reads rows with separate
`HANDLER ... READ` commands.

MyLite's public API is `libmylite` direct/prepared SQL over a file-owned
embedded session. Direct storage-engine cursor management outside ordinary SQL
planning does not fit the current API boundary, single-file lifecycle, or
storage-routing compatibility work. It also keeps table-handler hash, lock, and
cleanup code in the default embedded archive for a surface MyLite does not yet
support or test as a drop-in application requirement.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB documentation describes `HANDLER` statements as direct access
to table storage-engine interfaces for key lookups and table scans:
<https://mariadb.com/kb/en/handler-commands/>.

Source paths inspected:

- `mariadb/sql/CMakeLists.txt` and `mariadb/libmysqld/CMakeLists.txt` include
  `sql_handler.cc` in SQL and embedded SQL source lists.
- `mariadb/sql/lex.h` defines the `HANDLER` keyword token.
- `mariadb/sql/sql_yacc.yy` parses top-level `HANDLER tbl OPEN`,
  `HANDLER tbl CLOSE`, and `HANDLER tbl READ ...` syntax into
  `SQLCOM_HA_OPEN`, `SQLCOM_HA_CLOSE`, and `SQLCOM_HA_READ`.
- `mariadb/sql/sql_parse.cc` dispatches those commands through
  `mysql_ha_open()`, `mysql_ha_close()`, and `mysql_ha_read()`.
- `mariadb/sql/sql_handler.h` declares the `SQL_HANDLER` object and cleanup
  hooks used from retained server paths.
- `mariadb/sql/sql_handler.cc` implements handler hash management, direct table
  opening, handler reads, metadata-lock duration handling, flush cleanup, and
  temporary-table cleanup.

Historical branch-level bundle-size research measured this cut as 6,648 linked
bytes and 20,550 archive bytes saved. The current profile must be remeasured
because later server-surface trims changed the embedded archive and linked
roots.

## Design

- Add `MYLITE_WITH_SQL_HANDLER_COMMAND`, defaulting to `ON` for normal MariaDB
  builds and forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, replace `sql_handler.cc` with a small MyLite-owned
  disabled implementation that preserves `SQL_HANDLER` cleanup methods and
  `mysql_ha_*()` symbols.
- Keep parser tokens, grammar, command enum, parse dispatch, and handler header
  intact. This avoids generated parser churn and keeps retained cleanup calls
  link-safe.
- Reject public MyLite direct and prepared SQL whose first significant token is
  `HANDLER` before MariaDB execution with a stable MyLite diagnostic.
- Leave ordinary SQL table scans, indexed reads, prepared statements, and
  storage-engine routing unchanged.

## Affected Subsystems

- MariaDB SQL and embedded SQL build profile.
- SQL handler command runtime symbols.
- Public SQL policy and compatibility coverage.
- Compatibility harness grouping.
- Size-profile documentation and measurement.

## MySQL/MariaDB Compatibility Impact

Top-level SQL `HANDLER` commands become deliberately unsupported in the default
MyLite embedded profile. This is a compatibility tradeoff for a low-level
storage-engine access path rather than ordinary application SQL.

Stored-program `DECLARE ... HANDLER` remains covered by the existing stored
program and non-table-object policy. This slice is specifically about top-level
SQL handler cursor commands.

## Single-File And Embedded-Lifecycle Impact

No durable file-format change. The disabled stub does not open handler cursors,
create tables, create sidecars, or retain metadata locks. Cleanup hooks remain
link-safe no-ops.

## Public API And File-Format Impact

No MyLite C API or `.mylite` file-format change. Direct execution and prepared
statement preparation fail with `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB
errno, and a diagnostic that names SQL `HANDLER`.

## Storage-Engine Routing Impact

The slice removes a direct storage-engine cursor command surface from the
default embedded profile. Ordinary routed DDL/DML and handler/storage-engine
implementation APIs remain unchanged.

## Binary-Size Impact

Measured on 2026-05-16 after implementation:

- Default embedded archive:
  `build/mariadb-embedded/libmysqld/libmariadbd.a` is 27,327,168 bytes /
  26.06 MiB with 675 members, 13,424 bytes smaller than the previous
  dynamic-column-trim baseline.
- Opt-in storage-smoke archive:
  `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` is
  27,507,752 bytes / 26.23 MiB with 678 members, 13,424 bytes smaller than the
  previous dynamic-column-trim storage-smoke baseline.
- Both disabled-profile archives contain `mylite_sql_handler_disabled.cc.o`
  and omit `sql_handler.cc.o`.

## Implementation Notes

- The disabled stub should preserve `SQL_HANDLER::reset()`, destructor, and
  cleanup hooks so retained THD cleanup paths remain link-safe.
- Public policy should reject only top-level `HANDLER` commands so quoted text
  and unrelated uses of the word remain unaffected.
- Raw embedded MariaDB entry points should fail closed with `ER_NOT_SUPPORTED_YET`
  if they reach command execution despite MyLite policy rejection.

## License And Dependency Impact

No new dependency. The change replaces MariaDB-derived SQL handler command
runtime source with a small GPL-compatible MyLite disabled implementation in
the embedded disabled profile only. Normal MariaDB builds keep the upstream
implementation by default.

## Test And Verification Plan

- Add direct SQL policy coverage for `HANDLER ... OPEN`, `HANDLER ... READ`,
  `HANDLER ... CLOSE`, and executable-comment `HANDLER` rejection.
- Add prepared statement coverage for `HANDLER ... READ` rejection because
  MariaDB documents prepared `HANDLER READ` support.
- Add positive coverage proving quoted mentions remain allowed.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both disabled embedded archives omit `sql_handler.cc.o` and include
  the MyLite disabled SQL handler object.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Verification Results

Executed on 2026-05-16:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '^(sql_handler|mylite_sql_handler_disabled)\.cc\.o$'`
- `ar -t build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a | rg '^(sql_handler|mylite_sql_handler_disabled)\.cc\.o$'`
- `cmake --preset embedded-dev`
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `cmake --preset storage-smoke-dev`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --preset dev`
- `cmake --build --preset dev`
- `ctest --preset dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `tools/mylite-size-report`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report`
- `git diff --check`

All commands passed. Both archive inspections printed only
`mylite_sql_handler_disabled.cc.o`.

## Acceptance Criteria

- Public direct and prepared SQL reject top-level SQL `HANDLER` commands before
  MariaDB execution with stable MyLite diagnostics.
- Ordinary table scans and indexed DML continue to work in the default embedded
  profile.
- Default embedded and storage-smoke archives omit the upstream SQL handler
  runtime object and record size reductions.
- Normal MariaDB builds keep the default SQL `HANDLER` implementation.
- Documentation records the exact unsupported boundary and measurements.

## Risks And Open Questions

- Some low-level MariaDB applications may use `HANDLER` for direct engine
  scans. The current compatibility judgment is that this can remain unsupported
  until a concrete application suite needs it.
- The parser still recognizes `HANDLER`. MyLite policy rejection is the
  supported public boundary; the stub protects disabled embedded builds from
  unresolved symbols and accidental raw embedded entry.
