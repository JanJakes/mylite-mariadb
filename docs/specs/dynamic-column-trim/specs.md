# Dynamic Column Trim

## Problem

The default embedded profile still carries MariaDB's packed dynamic-column
implementation. Dynamic columns are a MariaDB-specific NoSQL-style feature:
SQL functions such as `COLUMN_CREATE()`, `COLUMN_GET()`, `COLUMN_ADD()`,
`COLUMN_DELETE()`, `COLUMN_CHECK()`, `COLUMN_EXISTS()`, `COLUMN_LIST()`, and
`COLUMN_JSON()` store and read variable attributes inside a BLOB.

That surface does not advance the current MyLite roadmap. MyLite is building
ordinary SQL table, index, transaction, recovery, and compatibility behavior
over one `.mylite` file. Packed dynamic columns are a parallel storage encoding
inside user BLOB values, not a prerequisite for file lifecycle, storage-engine
routing, or common MySQL application-schema compatibility.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB documentation describes dynamic columns as a way to store
different sets of attributes for each row in a BLOB and exposes both SQL
functions and a client-side dynamic-column API:

- <https://mariadb.com/docs/server/reference/sql-structure/nosql/dynamic-columns>
- <https://mariadb.com/docs/server/reference/sql-structure/nosql/dynamic-column-api>

Source paths inspected:

- `mariadb/mysys/CMakeLists.txt` compiles `ma_dyncol.c` into `mysys`.
- `mariadb/mysys/ma_dyncol.c` implements the packed dynamic-column encoding,
  decoding, JSON conversion, value conversion, listing, update, and cleanup
  routines.
- `mariadb/include/ma_dyncol.h` declares the exported
  `mariadb_dyncol_*()` API, deprecated `dynamic_column_*()` wrappers, packed
  value types, result codes, and decimal helper.
- `mariadb/libmysqld/CMakeLists.txt` lists dynamic-column symbols in the
  embedded client API export set.
- `mariadb/sql/lex.h` defines special keyword tokens for
  `COLUMN_ADD`, `COLUMN_CHECK`, `COLUMN_CREATE`, `COLUMN_DELETE`, and
  `COLUMN_GET`.
- `mariadb/sql/sql_yacc.yy` parses the special dynamic-column function syntax,
  including `COLUMN_GET(... AS type)` and `COLUMN_CREATE(... AS dynamic type)`.
- `mariadb/sql/item_create.cc` registers generic builders for
  `COLUMN_CHECK()`, `COLUMN_EXISTS()`, `COLUMN_LIST()`, and `COLUMN_JSON()`,
  and constructs item objects for the special dynamic-column syntax.
- `mariadb/sql/item_strfunc.cc` and `mariadb/sql/item_cmpfunc.cc` implement the
  SQL item classes that call the dynamic-column C API.

Historical branch-level bundle-size research measured this cut as 42,464
linked bytes and 176,088 archive bytes saved. The current profile must be
remeasured because later server-surface trims changed the embedded archive and
linked roots.

## Design

- Add `MYLITE_WITH_DYNAMIC_COLUMNS`, defaulting to `ON` for normal MariaDB
  builds and forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, replace `ma_dyncol.c` with a small MyLite-owned
  disabled implementation that preserves the exported API symbols and returns
  fail-closed dynamic-column result codes.
- Keep parser tokens, grammar, headers, and SQL item classes intact. This keeps
  the fork delta small and avoids generated grammar churn.
- Reject direct and prepared MyLite SQL using dynamic-column SQL functions
  before MariaDB execution with a stable MyLite diagnostic.
- Leave ordinary BLOB, JSON scalar/path helpers, table DDL, and row storage
  behavior unchanged.

## Affected Subsystems

- MariaDB `mysys` embedded build profile.
- Dynamic-column SQL policy in `libmylite`.
- Public compatibility matrix and API docs.
- Compatibility harness grouping.
- Size-profile documentation and measurement.

## MySQL/MariaDB Compatibility Impact

Dynamic columns become deliberately unsupported in the default MyLite embedded
profile. This is a MariaDB-specific compatibility tradeoff. Applications that
depend on packed dynamic-column blobs need a later compatibility decision
before MyLite exposes this surface.

MySQL compatibility is not reduced because MySQL does not provide MariaDB's
dynamic-column function family.

## Single-File And Embedded-Lifecycle Impact

No durable file-format change. The disabled C API stubs do not create tables,
handlers, sidecars, or temporary storage. Existing embedded lifecycle tests
must continue to prove repeated initialization and cleanup.

## Public API And File-Format Impact

No MyLite C API or `.mylite` file-format change. Direct execution and prepared
statement preparation fail with `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB
errno, and a diagnostic that names dynamic columns.

The inherited MariaDB dynamic-column C API symbols remain linkable in the
embedded archive, but the disabled profile returns dynamic-column error codes
instead of packing or unpacking values.

## Storage-Engine Routing Impact

No durable storage-engine routing change. Dynamic columns are BLOB payload
helpers rather than table-engine routing or catalog metadata.

## Binary-Size Impact

This slice records fresh default embedded and opt-in storage-smoke archive
measurements after implementation.

Measured on 2026-05-16 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,340,592 bytes / 26.07 MiB | 675 | -29,752 bytes, same member count |
| Storage-smoke | 27,521,176 bytes / 26.25 MiB | 678 | -29,752 bytes, same member count |

The disabled default and storage-smoke embedded archives omit `ma_dyncol.c.o`
and include `mylite_ma_dyncol_disabled.c.o`. The member count is unchanged
because the full runtime object is replaced by the disabled stub.

## Implementation Notes

- The disabled profile should avoid grammar edits and generated parser churn.
- The fail-closed C API should preserve cleanup helpers that are safe to call,
  such as dynamic-string cleanup and unpacked-array cleanup.
- Direct and prepared SQL policy should reject only dynamic-column function
  calls, while leaving quoted mentions alone.

## License And Dependency Impact

No new dependency. The change replaces MariaDB-derived dynamic-column runtime
source with a small GPL-compatible MyLite disabled implementation in the
embedded disabled profile only. Normal MariaDB builds keep the upstream
implementation by default.

## Test And Verification Plan

- Add direct SQL policy coverage for dynamic-column function rejection.
- Add prepared statement coverage for dynamic-column function rejection.
- Add positive coverage proving quoted mentions and ordinary retained helpers
  still work.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both disabled embedded archives omit `ma_dyncol.c.o` and include the
  MyLite disabled dynamic-column object.
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
- `ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '^(ma_dyncol|mylite_ma_dyncol_disabled)\.c\.o$'`
- `ar -t build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a | rg '^(ma_dyncol|mylite_ma_dyncol_disabled)\.c\.o$'`
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

The archive scans printed only `mylite_ma_dyncol_disabled.c.o` for both
disabled profiles, confirming that `ma_dyncol.c.o` is absent and the MyLite
stub is present.

## Acceptance Criteria

- Public direct and prepared SQL reject dynamic-column SQL functions before
  MariaDB execution with stable MyLite diagnostics.
- Ordinary BLOB values, retained JSON helpers, and current schema/storage
  behavior continue to work in the default embedded profile.
- Default embedded and storage-smoke archives omit the upstream dynamic-column
  runtime object and record size reductions.
- Normal MariaDB builds keep the default dynamic-column implementation.
- Documentation records the exact unsupported boundary and measurements.

## Risks And Open Questions

- Some MariaDB-specific applications may use dynamic columns as a semi-structured
  attribute store. The current compatibility judgment is that this can remain
  unsupported until a concrete application suite needs it.
- The parser still recognizes dynamic-column syntax. MyLite policy rejection is
  the supported public boundary; the disabled C API only protects disabled
  embedded builds from unresolved symbols and accidental raw embedded entry.
