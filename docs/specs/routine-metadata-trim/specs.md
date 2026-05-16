# Routine Metadata Trim

## Problem

The default MyLite embedded profile rejects stored routine, package, trigger,
event, and `CALL` execution, and it links a fail-closed stored-program runtime
stub. It still retains MariaDB's routine metadata readers that scan
`mysql.proc` for `SHOW PROCEDURE STATUS`, `SHOW FUNCTION STATUS`,
`INFORMATION_SCHEMA.ROUTINES`, and `INFORMATION_SCHEMA.PARAMETERS`.

MyLite has no catalog-backed routine metadata yet, and inherited `mysql.proc`
scans are not part of the file-owned single-file runtime.

## Source Findings

Base: MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- MariaDB documents `SHOW PROCEDURE STATUS` as a MariaDB extension that lists
  stored procedures and notes that `INFORMATION_SCHEMA.ROUTINES` contains more
  detailed routine information:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-procedure-status>.
- MariaDB documents `SHOW FUNCTION STATUS` as the corresponding stored
  function status command, also backed by `INFORMATION_SCHEMA.ROUTINES`:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-function-status>.
- MariaDB documents `INFORMATION_SCHEMA.ROUTINES` as storing stored procedure
  and stored function definitions and properties:
  <https://mariadb.com/docs/server/reference/system-tables/information-schema/information-schema-tables/information-schema-routines-table>.
- MariaDB documents `INFORMATION_SCHEMA.PARAMETERS` as storing stored routine
  parameter metadata and describing its relationship to `mysql.proc` and
  `SHOW CREATE PROCEDURE` / `SHOW CREATE FUNCTION`:
  <https://mariadb.com/docs/server/reference/system-tables/information-schema/information-schema-tables/information-schema-parameters-table>.
- `mariadb/sql/sql_yacc.yy` parses `SHOW PROCEDURE STATUS`,
  `SHOW FUNCTION STATUS`, `SHOW PACKAGE STATUS`, and
  `SHOW PACKAGE BODY STATUS` by preparing `SCH_PROCEDURES`.
- `mariadb/sql/sql_show.cc` registers `PARAMETERS` as
  `SCH_PARAMETERS` and `ROUTINES` as `SCH_PROCEDURES`.
- `mariadb/sql/sql_show.cc` implements `fill_schema_proc()` by opening
  `mysql.proc` with `open_proc_table_for_read()`, walking the routine index,
  and dispatching to `store_schema_proc()` or `store_schema_params()`.
- `store_schema_proc()` and `store_schema_params()` load stored routine
  metadata through `Sp_handler` and stored-program parse helpers.

## Design

- Add `MYLITE_WITH_ROUTINE_METADATA`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When disabled, compile out the `mysql.proc` routine metadata scan helpers in
  `sql_show.cc`.
- Keep the `ROUTINES` and `PARAMETERS` information-schema table definitions
  registered so metadata probes get stable table shapes.
- Route `ROUTINES` and `PARAMETERS` to a MyLite empty schema-table filler when
  disabled. As a result, `SHOW PROCEDURE STATUS`, `SHOW FUNCTION STATUS`, and
  package-status variants return empty result sets through MariaDB's existing
  SHOW-to-information-schema path.
- Keep public `libmylite` rejection for routine object creation, mutation,
  execution, and `SHOW CREATE` surfaces. This slice does not introduce
  catalog-backed routine storage or routine execution.

## Affected Subsystems

- MariaDB embedded build profile and `sql_show.cc` metadata producers.
- Public direct-SQL compatibility tests for routine metadata and non-table
  object rejection.
- Compatibility harness labels, compatibility matrix, API docs, roadmap, and
  embedded-build size documentation.

## Compatibility Impact

Stored routines remain unsupported in the core embedded profile. Routine status
and information-schema metadata become deterministic empty metadata surfaces
until MyLite has a routine catalog. That intentionally differs from MariaDB
Server when routines exist, but it avoids accidental `mysql.proc` scans while
preserving familiar metadata table shapes for probes.

## DDL Metadata Routing Impact

No routine catalog is added. This slice prevents inherited routine metadata
from reading server-owned `mysql.proc` state and leaves routine DDL rejected
before MariaDB can publish routine metadata.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companions. The disabled profile does not
read or write routine metadata side tables for status or information-schema
queries.

## Public API And File-Format Impact

No C API or `.mylite` format change. `mylite_exec()` can query
`INFORMATION_SCHEMA.ROUTINES`, `INFORMATION_SCHEMA.PARAMETERS`,
`SHOW PROCEDURE STATUS`, and `SHOW FUNCTION STATUS`, and gets zero routine
rows. Routine object DDL, `CALL`, and `SHOW CREATE` remain explicit
unsupported-surface errors.

## Storage-Engine Routing Impact

None. Routine metadata is a server catalog surface, not table handler routing.

## Wire-Protocol Or Integration-Package Impact

Future protocol or adapter layers should inherit the same policy unless they
add a separate routine-catalog implementation around the core library.

## Binary-Size Impact

The bundle-size research estimated this as a small source trim. The retained
information-schema field metadata and SHOW formatting keep the impact modest.

Measured on 2026-05-16:

| Profile | Archive | Size | Members | Delta From Previous Baseline |
| --- | --- | ---: | ---: | ---: |
| Default embedded | `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,926,688 bytes / 25.68 MiB | 670 | -14,208 bytes, same members |
| Storage-smoke | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` | 27,107,272 bytes / 25.85 MiB | 673 | -14,208 bytes, same members |

## License And Dependency Impact

No new dependency and no license change. The slice only compiles out retained
MariaDB GPL-2.0 source paths in the embedded profile.

## Test And Verification Plan

- Add direct SQL tests proving `INFORMATION_SCHEMA.ROUTINES` and
  `INFORMATION_SCHEMA.PARAMETERS` remain queryable and return zero rows.
- Add direct SQL tests proving `SHOW PROCEDURE STATUS` and
  `SHOW FUNCTION STATUS` return zero rows.
- Add direct and prepared SQL tests proving routine `SHOW CREATE` remains
  rejected through the non-table object policy.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_ROUTINE_METADATA=OFF`.
- `SHOW PROCEDURE STATUS`, `SHOW FUNCTION STATUS`,
  `INFORMATION_SCHEMA.ROUTINES`, and `INFORMATION_SCHEMA.PARAMETERS` remain
  visible with zero rows.
- Routine object DDL, `CALL`, and `SHOW CREATE` stay rejected before MariaDB
  execution through public MyLite APIs.
- Supported SQL execution, table metadata, and storage-routing tests continue
  to pass.
- Size measurements and compatibility documentation are updated.

## Risks

- Some applications use routine metadata probes to detect installed stored
  procedures. Empty result sets are acceptable only while routine creation is
  explicitly unsupported.
- Package-status SHOW commands share the same `SCH_PROCEDURES` path. Empty
  package status is intentional because packages are also unsupported.
