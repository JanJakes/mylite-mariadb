# JSON Table Function Trim

## Problem

The default embedded profile still carries MariaDB's `JSON_TABLE` table-function
execution path. MyLite keeps ordinary JSON scalar and path helpers because real
application SQL often uses them in projections, predicates, generated columns,
and CHECK constraints. `JSON_TABLE` is a heavier table-reference surface that
materializes JSON input as a relational table through a dedicated handler and
temporary table setup path.

That behavior does not advance the current single-file storage, embedded
lifecycle, or application-schema roadmap. It also keeps a table-function
execution object in the default archive for a feature MyLite does not yet
support or test as a drop-in application requirement.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB documentation describes `JSON_TABLE` as a table function that
converts JSON into relational columns and can appear where a table reference is
valid, including `FROM` and multi-table `UPDATE` / `DELETE`:
<https://mariadb.com/docs/server/reference/sql-functions/special-functions/json-functions/json_table>.

- `mariadb/sql/CMakeLists.txt` and `mariadb/libmysqld/CMakeLists.txt` include
  `json_table.cc` in the SQL and embedded SQL source lists.
- `mariadb/sql/lex.h` declares the `JSON_TABLE` keyword token.
- `mariadb/sql/sql_yacc.yy` parses the `JSON_TABLE(...)` table-function syntax,
  allocates `Table_function_json_table`, records it on `TABLE_LIST`, and sets
  the statement unsafe for statement-based binlogging.
- `mariadb/sql/json_table.h` declares `Json_table_column`,
  `Json_table_nested_path`, `Table_function_json_table`,
  `push_table_function_arg_context()`, `create_table_for_function()`, and
  `add_table_function_dependencies()`.
- `mariadb/sql/json_table.cc` implements the dedicated `ha_json_table` handler,
  temporary table construction, JSON path scans, column extraction, optimizer
  estimates, print support, and table-function dependency calculation.
- Retained optimizer, ACL, table, view, subselect, and explain paths refer to
  `TABLE_LIST::table_function` and call methods declared in `json_table.h`.
  Removing `json_table.cc` therefore needs link-safe disabled-profile stubs,
  not just a source-list deletion.
- Historical branch-level bundle-size research measured a comparable
  `JSON_TABLE` execution trim as 10,224 linked bytes and 115,620 archive bytes
  saved while retaining ordinary JSON scalar functions. The current profile
  must be remeasured because later server-surface trims have changed the
  archive and linked roots.

## Design

- Add `MYLITE_WITH_JSON_TABLE`, defaulting to `ON` for normal MariaDB builds and
  forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, remove `json_table.cc` from the SQL and embedded
  SQL source lists and compile a small MyLite-owned disabled stub instead.
- Keep `json_table.h`, the parser token, and table-function fields intact.
  That preserves upstream parser and optimizer shapes and keeps the fork delta
  small.
- Make the disabled stub link-safe and fail closed if raw embedded MariaDB code
  reaches the table-function path despite MyLite policy rejection.
- Reject direct and prepared MyLite SQL that calls `JSON_TABLE(...)` before
  MariaDB execution with a stable MyLite diagnostic.
- Keep ordinary JSON helpers, including `JSON_VALID()` and `JSON_EXTRACT()`,
  available.

## Affected Subsystems

- MariaDB SQL and embedded SQL build profile.
- Parser-linked JSON table-function runtime symbols.
- Public SQL policy and compatibility coverage.
- Size-profile documentation and measurement.

## MySQL/MariaDB Compatibility Impact

`JSON_TABLE` is deliberately unsupported in the default MyLite embedded profile.
This is a narrower tradeoff than removing ordinary JSON scalar/path helpers:
`JSON_TABLE` is a table-reference transformation feature, while scalar JSON
helpers are more likely to appear in application schemas and generated SQL.

Applications that depend on relational projection of JSON documents need a
later compatibility decision before the table function can be exposed.

## Single-File And Embedded-Lifecycle Impact

No durable file-format change. The disabled stub does not create tables,
handlers, sidecars, or temporary storage. Existing embedded lifecycle tests must
continue to prove repeated initialization and cleanup.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Direct execution and prepared
statement preparation fail with `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB
errno, and a diagnostic that names `JSON_TABLE`.

## Storage-Engine Routing Impact

No durable storage-engine routing change. The retained parser shape still knows
about table functions, but public MyLite SQL does not allow `JSON_TABLE` to
reach MariaDB execution in the disabled profile.

## Binary-Size Impact

Historical branch-level research suggests a modest archive and linked-runtime
reduction. This slice records fresh default embedded and opt-in storage-smoke
archive measurements after implementation.

Measured on 2026-05-16 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,370,344 bytes / 26.10 MiB | 675 | -44,400 bytes, same member count |
| Storage-smoke | 27,550,928 bytes / 26.27 MiB | 678 | -44,400 bytes, same member count |

The disabled default and storage-smoke embedded archives omit
`json_table.cc.o` and include `mylite_json_table_disabled.cc.o`. The member
count is unchanged because the full runtime object is replaced by the disabled
stub.

## Implementation Notes

- The disabled profile should use MyLite-owned stub symbols rather than editing
  the generated grammar or removing `json_table.h`.
- The stub should preserve upstream signatures and return errors, empty
  dependency sets, or conservative estimates without allocating a handler.
- Direct and prepared SQL policy should reject `JSON_TABLE` only when followed
  by a function call parenthesis, while leaving quoted mentions alone.

## License And Dependency Impact

No new dependency. The change removes MariaDB-derived table-function execution
source from the disabled embedded profile only and keeps normal MariaDB
defaults intact.

## Test And Verification Plan

- Add direct SQL policy coverage for `JSON_TABLE(...)` rejection.
- Add prepared statement coverage for `JSON_TABLE(...)` rejection.
- Add positive direct and prepared coverage proving ordinary JSON scalar
  helpers remain available.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both disabled embedded archives omit `json_table.cc.o` and include
  the MyLite disabled stub.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Verification Results

Completed on 2026-05-16:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '^(json_table|mylite_json_table_disabled)\.cc\.o$'`
- `ar -t build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a | rg '^(json_table|mylite_json_table_disabled)\.cc\.o$'`
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
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report`
- `git diff --check`

The archive scans printed only `mylite_json_table_disabled.cc.o` for both
disabled profiles, confirming that `json_table.cc.o` is absent and the MyLite
stub is present.

## Acceptance Criteria

- Public direct and prepared SQL reject `JSON_TABLE(...)` before MariaDB
  execution with stable MyLite diagnostics.
- `JSON_VALID()` and ordinary retained JSON helpers continue to work in the
  default embedded profile.
- Default embedded and storage-smoke archives omit the disabled JSON table
  runtime object and record size reductions.
- Normal MariaDB builds keep the default `JSON_TABLE` implementation.
- Documentation records the exact unsupported boundary and measurements.

## Risks And Open Questions

- Some applications use `JSON_TABLE` for report-style projections or
  application-side import flows. The current compatibility judgment is that it
  can remain unsupported until a concrete application suite needs it.
- The parser still recognizes `JSON_TABLE`. MyLite policy rejection is the
  supported public boundary; the stub only protects disabled embedded builds
  from unresolved symbols and accidental raw embedded entry.
