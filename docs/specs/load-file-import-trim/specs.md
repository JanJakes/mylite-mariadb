# Load File Import Trim

## Problem Statement

The embedded profile should not expose SQL that reads arbitrary host files or
client-protocol file streams. `LOAD DATA` and `LOAD XML` are server filesystem
or wire-protocol import surfaces, not core application DML, storage-engine
behavior, or `libmylite` parameter binding.

Normal application loading remains available through ordinary `INSERT`,
prepared statement bindings, and `INSERT ... SELECT`.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `LOAD DATA` and `LOAD XML` into
  `SQLCOM_LOAD`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_LOAD` to `mysql_load()`.
- `mariadb/sql/sql_prepare.cc` prepares `SQLCOM_LOAD` through the common insert
  preparation path, so MyLite must reject prepared imports before MariaDB
  preparation.
- `mariadb/sql/sql_load.cc` implements `mysql_load()`, host-file reads,
  `LOAD DATA LOCAL` client-file handling, XML row parsing, and
  `Load_data_param` helper methods.
- Under `EMBEDDED_LIBRARY`, MariaDB already clears the client-file branch for
  `LOAD DATA LOCAL`, but the SQL surface still describes file import outside
  the `libmylite` result and parameter API.

## Design

Add `MYLITE_WITH_LOAD_FILE_IMPORTS`, defaulting to `ON` for normal MariaDB
builds and forced `OFF` in the MyLite embedded baseline. When disabled:

- replace `sql_load.cc` with `mylite_sql_load_disabled.cc` in the embedded
  archive;
- keep the small `Load_data_param` helper methods needed by retained parser
  and item code;
- compile `mysql_load()` to a fail-closed unsupported stub;
- reject direct and prepared `LOAD DATA` and `LOAD XML` statements through the
  MyLite SQL policy before MariaDB dispatch or preparation;
- keep ordinary `INSERT`, prepared bindings, and `INSERT ... SELECT` unchanged.

The `local_infile` system variable is not removed in this slice. The supported
contract is enforced by the SQL policy and by omitting the execution runtime.

## Compatibility Impact

Host-file SQL imports become explicitly unsupported in the default embedded
profile. This is a server filesystem and client-protocol surface, so it does
not fit the serverless directory-owned core. Applications should load data
through prepared bindings, multi-row `INSERT`, or application-owned file
parsing.

## Directory And Lifecycle Impact

No durable engine files or database-directory layout changes are introduced.
Rejecting file imports prevents the core API from reading caller-named files
outside the MyLite database directory or accepting protocol file streams.

## Native Storage Impact

None. Native engine inserts, `INSERT ... SELECT`, table reads, indexes, and
transactions continue through retained MariaDB paths.

## Binary-Size Impact

The slice replaces `sql_load.cc.o` with `mylite_sql_load_disabled.cc.o`. On
this branch, `tools/mariadb-embedded-build all` reduced the stripped archive
from 26,077,728 bytes / 24.87 MiB to 26,053,336 bytes / 24.85 MiB with the
member count unchanged at 693.

## Test And Verification Plan

- Confirm `MYLITE_WITH_LOAD_FILE_IMPORTS=OFF` appears in the embedded CMake
  cache.
- Confirm `sql_load.cc.o` is absent and `mylite_sql_load_disabled.cc.o` is
  present in `libmariadbd.a`.
- Confirm direct and prepared `LOAD DATA INFILE`, `LOAD DATA LOCAL INFILE`,
  and `LOAD XML INFILE` return the stable MyLite unsupported-surface
  diagnostic.
- Confirm quoted string literals containing `LOAD DATA` remain ordinary SQL.
- Run embedded and first-party build, CTest, format, tidy, and size measurement
  checks.

## Acceptance Criteria

- The embedded archive builds with host-file import runtime disabled.
- Public MyLite policy rejects direct and prepared file-import SQL forms.
- Ordinary `INSERT`, prepared bindings, `INSERT ... SELECT`, JSON,
  GEOMETRY/GIS, native storage, DDL/DML, transactions, and prepared statements
  remain covered.
- Docs and compatibility matrix describe the unsupported boundary.
