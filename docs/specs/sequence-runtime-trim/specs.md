# Sequence Runtime Trim

## Problem

MyLite already rejects sequence objects as unsupported non-table database
objects, but the default embedded profile still links MariaDB's SQL sequence
runtime and SQL_SEQUENCE storage-engine plugin.

MariaDB sequences are independent schema objects backed by a special sequence
table shape and a hidden storage engine. MyLite does not yet have catalog-backed
sequence metadata, sequence value persistence, dependency tracking, or
replication semantics. Keeping the runtime in the embedded profile preserves a
surface that the public API deliberately rejects.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB documentation describes `CREATE SEQUENCE` as creating a schema
object that generates values through `NEXT VALUE FOR` / `NEXTVAL()`, with
`SETVAL()` used to update the next returned value:

- <https://mariadb.com/docs/server/reference/sql-structure/sequences/create-sequence>
- <https://mariadb.com/docs/server/reference/sql-structure/sequences/sequence-functions/next-value-for-sequence_name>
- <https://mariadb.com/docs/server/reference/sql-structure/sequences/sequence-functions/setval>

Source paths inspected:

- `mariadb/sql/CMakeLists.txt` includes `sql_sequence.cc` in SQL sources and
  registers `sql_sequence` from `ha_sequence.cc` as a mandatory static storage
  engine.
- `mariadb/libmysqld/CMakeLists.txt` includes both `sql_sequence.cc` and
  `ha_sequence.cc` in the embedded SQL source list.
- `mariadb/sql/sql_yacc.yy` parses `CREATE SEQUENCE`, `ALTER SEQUENCE`,
  `DROP SEQUENCE`, `NEXT VALUE FOR`, `PREVIOUS VALUE FOR`, `NEXTVAL()`,
  `LASTVAL()`, and `SETVAL()`.
- `mariadb/sql/sql_sequence.h` declares `sequence_definition`, `SEQUENCE`,
  `SEQUENCE_LAST_VALUE`, `check_sequence_fields()`, and `sequence_insert()`.
- `mariadb/sql/sql_sequence.cc` implements sequence metadata validation,
  initial row publication, value allocation, `ALTER SEQUENCE`, and last-value
  caching.
- `mariadb/sql/ha_sequence.cc` implements the hidden SQL_SEQUENCE storage-engine
  wrapper and defines `sql_sequence_hton`.
- Retained MariaDB code references sequence methods from normal table paths,
  `NEXTVAL()` / `SETVAL()` item execution, binlog event helpers, table
  truncation, table metadata loading, and DDL helpers.

Historical branch-level bundle-size research measured this cut as 14,376
linked bytes and 217,508 archive bytes saved. The current profile must be
remeasured because later profile trims changed the embedded archive and linked
roots.

## Design

- Add `MYLITE_WITH_SEQUENCE_RUNTIME`, defaulting to `ON` for normal MariaDB
  builds and forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, replace `sql_sequence.cc` / `ha_sequence.cc` with a
  small MyLite-owned disabled implementation that preserves retained sequence
  symbols and the `sql_sequence_hton` global.
- When the option is `OFF`, do not register the `sql_sequence` storage-engine
  plugin in `mariadb/sql/CMakeLists.txt`.
- Keep sequence parser grammar and headers intact. This avoids generated parser
  churn and keeps retained references link-safe.
- Continue rejecting `CREATE SEQUENCE`, `ALTER SEQUENCE`, and `DROP SEQUENCE`
  through the existing non-table-object policy.
- Add public policy rejection for sequence value functions and expressions:
  `NEXT VALUE FOR`, `PREVIOUS VALUE FOR`, `NEXTVAL()`, `LASTVAL()`, and
  `SETVAL()`.

## Affected Subsystems

- MariaDB SQL and embedded SQL build profile.
- SQL_SEQUENCE storage-engine plugin registration.
- Retained sequence runtime symbols.
- Public SQL policy and compatibility coverage.
- Compatibility, API, roadmap, and size-profile documentation.

## MySQL/MariaDB Compatibility Impact

MariaDB sequence objects and value functions remain deliberately unsupported in
the default MyLite embedded profile. This is already true for object DDL through
the non-table-object policy; this slice extends the same explicit boundary to
sequence value functions that otherwise reach retained MariaDB runtime.

`AUTO_INCREMENT` remains the supported MySQL/MariaDB-compatible value-generation
surface for routed tables.

## Single-File And Embedded-Lifecycle Impact

No durable file-format change. The disabled runtime does not publish sequence
metadata, create sequence engine files, allocate sequence values, or retain
sequence locks. Existing public rejection prevents sequence object side effects
before MariaDB execution.

## Public API And File-Format Impact

No MyLite C API or `.mylite` file-format change. Direct execution and prepared
statement preparation fail with `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB
errno, and a diagnostic that names SQL sequence support for sequence value
functions.

## Storage-Engine Routing Impact

The slice removes the hidden SQL_SEQUENCE storage-engine plugin from the
default embedded profile. Ordinary MyLite table storage, requested engine
routing, `AUTO_INCREMENT`, and supported DDL/DML are unchanged.

## Binary-Size Impact

Measured on 2026-05-16:

- Default embedded archive: 27,226,016 bytes / 25.96 MiB, 673 members.
- Opt-in storage-smoke archive: 27,406,592 bytes / 26.14 MiB, 676 members.

Relative to the preceding SQL HANDLER trim, the sequence runtime trim reduced
the default archive by 101,152 bytes and two members, and reduced the
storage-smoke archive by 101,160 bytes and two members. Both disabled archives
omit `sql_sequence.cc.o` and `ha_sequence.cc.o` and include
`mylite_sql_sequence_disabled.cc.o`.

## Implementation Notes

- Disabled stubs should return unsupported errors if raw MariaDB entry points
  are reached despite MyLite SQL policy rejection.
- Stubs for `SEQUENCE` methods must preserve link safety for retained
  `NEXTVAL()` / `SETVAL()` item code and table helpers.
- Public policy should detect sequence value functions across ordinary and
  executable-comment SQL while preserving quoted mentions.

## License And Dependency Impact

No new dependency. The change replaces MariaDB-derived sequence runtime source
with a small GPL-compatible MyLite disabled implementation in the embedded
disabled profile only. Normal MariaDB builds keep upstream sequence support by
default.

## Test And Verification Plan

- Add direct SQL policy coverage for `NEXT VALUE FOR`, `PREVIOUS VALUE FOR`,
  `NEXTVAL()`, `LASTVAL()`, and `SETVAL()` rejection.
- Add prepared statement coverage for representative sequence value function
  rejection.
- Keep existing `CREATE SEQUENCE` non-table-object coverage passing.
- Add positive coverage proving quoted sequence-function names remain allowed.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both disabled embedded archives omit `sql_sequence.cc.o` and
  `ha_sequence.cc.o` and include the MyLite disabled sequence object.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- Public direct and prepared SQL reject sequence value functions before MariaDB
  execution with stable MyLite diagnostics.
- Existing sequence object DDL rejection remains stable.
- Default embedded and storage-smoke archives omit upstream sequence runtime
  objects and record size reductions.
- Normal MariaDB builds keep default sequence support.
- Documentation records the exact unsupported boundary and measurements.

## Risks And Open Questions

- Some MariaDB applications use sequences instead of `AUTO_INCREMENT`. Current
  MyLite compatibility keeps them unsupported until catalog-backed sequence
  metadata, value persistence, and dependency behavior are designed.
- The parser still recognizes sequence syntax. MyLite policy rejection is the
  supported public boundary; disabled stubs protect disabled embedded builds
  from unresolved symbols and accidental raw embedded entry.

## Verification Results

Completed on 2026-05-16:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- archive scan confirming `mylite_sql_sequence_disabled.cc.o` is present and
  `sql_sequence.cc.o` / `ha_sequence.cc.o` are absent
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- storage-smoke archive scan confirming the same sequence object replacement
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
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness tools/mylite-size-report tools/mylite-mtr-harness`
- `git diff --check`
