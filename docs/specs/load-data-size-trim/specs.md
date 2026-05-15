# LOAD Data Size Trim

## Problem

MyLite now rejects `LOAD DATA` and `LOAD XML` at the public SQL policy
boundary. Keeping MariaDB's full `sql_load.cc` execution object in the
embedded archive therefore preserves file-reader, XML-reader, duplicate-mode,
and binlog-load query code that no supported core API path should reach.

This slice removes that execution object from the MyLite embedded profile while
keeping the upstream default build behavior available.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/CMakeLists.txt` includes `sql_load.cc` in the normal server
  `sql` library source list.
- `mariadb/libmysqld/CMakeLists.txt` has a separate `SQL_EMBEDDED_SOURCES`
  list and also includes `../sql/sql_load.cc`; the embedded archive is built
  from this list rather than reusing the normal `sql` target.
- `mariadb/sql/sql_parse.cc:4900-4924` is the only ordinary SQL execution
  caller of `mysql_load()` in the current source tree.
- `mariadb/sql/sql_prepare.cc:2260-2264` prepares `SQLCOM_LOAD` through the
  insert-common path, but MyLite rejects prepared LOAD statements before
  MariaDB prepare.
- `mariadb/sql/item.cc` and `mariadb/sql/item_func.cc` retain references to
  `Load_data_param::add_outvar_field()` and
  `Load_data_param::add_outvar_user_var()` through generic LOAD out-parameter
  virtual methods, so a linkable stub must keep those small helper methods even
  when the heavy executor is omitted.
- `docs/architecture/bundle-size-research.md` ranked "Remove `LOAD DATA` and
  `LOAD XML` execution object while keeping ordinary insert paths" as a passing
  historical trim.

## Scope

- Add a `MYLITE_WITH_LOAD_DATA` CMake option that defaults to `ON`.
- Set `MYLITE_WITH_LOAD_DATA=OFF` in the MyLite embedded profile.
- Replace `sql_load.cc` with a MyLite-owned fail-closed stub in both the normal
  `sql` target and the embedded `sql_embedded` source list when the option is
  off.
- Keep the parser, command enum, and `mysql_load()` symbol so upstream code
  still links.
- Keep `Load_data_param` helper methods required by retained generic item code.
- Record the cache option in `tools/mariadb-embedded-build measure`.

## Non-Goals

- Remove LOAD grammar from `sql_yacc.yy`.
- Remove `SQLCOM_LOAD` metadata from parser, lexer, or status arrays.
- Change MyLite public SQL diagnostics; the public policy gate from the
  file-import slice remains the user-facing behavior.
- Remove network-protocol client `LOAD DATA LOCAL` support from Connector/C
  sources.
- Apply broader file-import/output trims such as `SELECT ... INTO OUTFILE` or
  `LOAD_FILE()`.

## Design

Introduce `mariadb/sql/mylite_sql_load_disabled.cc` with the same public
`mysql_load()` symbol as `sql_load.cc`. The stub reports
`ER_NOT_SUPPORTED_YET` if a non-MyLite caller reaches it. MyLite's supported
entry points should never expose that diagnostic because `mylite_exec()` and
`mylite_prepare()` reject LOAD before MariaDB execution.

Both MariaDB source lists need the switch:

- `mariadb/sql/CMakeLists.txt` for the normal `sql` target;
- `mariadb/libmysqld/CMakeLists.txt` for the embedded archive actually used by
  MyLite tests.

The CMake option defaults to `ON` so an upstream-style build remains unchanged
unless a profile opts into the trim. The MyLite embedded baseline sets it to
`OFF`.

## Compatibility Impact

No supported MyLite behavior changes. Direct and prepared `LOAD DATA` /
`LOAD XML` remain explicitly rejected with stable MyLite diagnostics. Ordinary
`INSERT`, prepared DML, CTAS, storage-engine routing, and row storage are
unchanged.

## Single-File And Embedded-Lifecycle Impact

The trim reduces unreachable embedded code and does not add or remove durable
files. It reinforces the single-file policy by keeping server/client file
import outside the embedded core.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

None. LOAD statements do not reach handler write paths through supported MyLite
entry points.

## Wire-Protocol Or Integration-Package Impact

None for the core library. A future wire-protocol adapter that wants
`LOAD DATA LOCAL` compatibility must provide a separate controlled import
design instead of depending on the trimmed core profile.

## Binary-Size Impact

Measured on 2026-05-15 on macOS arm64 with Apple clang 21.0.0:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| Baseline embedded archive | 32,058,560 bytes | 32,024,568 bytes | -33,992 bytes |
| Storage-smoke embedded archive | 32,201,384 bytes | 32,167,392 bytes | -33,992 bytes |
| Stripped embedded open-close smoke | 17,660,144 bytes | 17,642,352 bytes | -17,792 bytes |
| Stripped storage-engine smoke | 17,958,304 bytes | 17,940,544 bytes | -17,760 bytes |

The linked-runtime saving is smaller than the historical branch measurement
but directionally positive and backed by the current branch's size report.

## Test And Verification Plan

- Reconfigure and rebuild `build/mariadb-embedded` with the baseline profile.
- Reconfigure and rebuild `build/mariadb-mylite-storage-smoke` with
  `-DPLUGIN_MYLITE_SE=STATIC`.
- Confirm both archives contain `mylite_sql_load_disabled.cc.o` and not
  `sql_load.cc.o`.
- Rebuild and run the embedded-dev and storage-smoke-dev CTest presets.
- Run `tools/mylite-compat-harness report server-surface`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, and first-party tidy checks.

## Acceptance Criteria

- MyLite embedded profile archives omit `sql_load.cc.o`.
- MyLite tests still reject direct and prepared LOAD statements at the public
  SQL policy boundary.
- Ordinary insert, prepared statement, CTAS, and storage-engine smoke coverage
  continues to pass.
- Size documentation records current branch evidence.
- Upstream-style MariaDB builds can retain LOAD execution by leaving
  `MYLITE_WITH_LOAD_DATA=ON`.

## Risks And Open Questions

- The parser still accepts LOAD syntax internally; this is deliberate to keep
  the trim small. MyLite's public SQL gate remains mandatory.
- The stub's `ER_NOT_SUPPORTED_YET` diagnostic is not the public MyLite
  contract. If a future adapter bypasses `libmylite` policy, it must either
  add its own compatibility behavior or reject LOAD explicitly.
- Additional file import/export trims such as `LOAD_FILE()` and
  `SELECT ... INTO OUTFILE` should be separate slices with their own source
  references and compatibility gates.
