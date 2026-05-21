# SELECT Into File Trim

## Problem Statement

The embedded profile should not expose SQL that writes arbitrary host files.
`SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` are server filesystem
export surfaces, not core application SQL, storage-engine behavior, or
`libmylite` result delivery. `SELECT ... INTO` variables remain useful SQL
behavior and must stay supported.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `INTO OUTFILE` into `select_export` and
  `INTO DUMPFILE` into `select_dump`, while `select_var_list_init` handles
  variable targets separately.
- `mariadb/sql/sql_class.cc` implements `select_to_file`, `select_export`, and
  `select_dump`. Those paths create host files with `mysql_file_create()` and
  write result rows through `IO_CACHE`.
- `mariadb/sql/sql_class.h` documents `sql_exchange` as the shared descriptor
  for non-database files used by `SELECT ... INTO OUTFILE` and `LOAD DATA`.
  The safe cut must not remove `sql_exchange`, because `LOAD DATA` still uses
  it.
- Historical size research ranked this as a small but safe cleanup after larger
  daemon-only trims. The old measurement saved about 19 KiB from the archive
  and kept `SELECT ... INTO` variables working.

## Design

Add `MYLITE_WITH_SELECT_INTO_FILE`, defaulting to `ON` for normal MariaDB
builds and set to `OFF` in the MyLite embedded baseline. When disabled:

- keep `sql_exchange` and ordinary `SELECT` result delivery intact;
- compile `select_to_file`, `select_export`, and `select_dump` to small
  fail-closed methods;
- reject direct and prepared `SELECT ... INTO OUTFILE` and
  `SELECT ... INTO DUMPFILE` statements through the MyLite SQL policy before
  MariaDB dispatch;
- keep `SELECT ... INTO @variable` covered as supported behavior.

## Compatibility Impact

Host-file SELECT exports become explicitly unsupported in the default embedded
profile. This is a server filesystem surface and can write outside the MyLite
database directory, so it does not fit the serverless directory-owned core.
Normal SQL result sets, prepared statements, and `SELECT ... INTO` variables
remain supported.

## Directory And Lifecycle Impact

No durable engine files or database-directory layout changes are introduced.
Rejecting host-file exports prevents MyLite from creating user-named files
outside the database directory.

## Native Storage Impact

None. Native engine table reads, result delivery, and variable assignment are
unchanged.

## Binary-Size Impact

The slice removes host-file writer bodies from the embedded `sql_class.cc`
object. On this branch, `tools/mariadb-embedded-build all` reduced the stripped
archive from 26,275,784 bytes / 25.06 MiB to 26,269,664 bytes / 25.05 MiB with
the member count unchanged at 698.

## Test And Verification Plan

- Confirm `MYLITE_WITH_SELECT_INTO_FILE=OFF` appears in the embedded CMake
  cache.
- Confirm direct and prepared `SELECT ... INTO OUTFILE` and
  `SELECT ... INTO DUMPFILE` return the stable MyLite unsupported-surface
  diagnostic.
- Confirm `SELECT ... INTO @variable` still succeeds.
- Run embedded and first-party build, CTest, format, tidy, and size measurement
  checks.

## Acceptance Criteria

- The embedded archive builds with host-file SELECT writer bodies disabled.
- Public MyLite policy rejects direct and prepared file-writing SELECT forms.
- `SELECT ... INTO` variables remain supported.
- Docs and compatibility matrix describe the unsupported boundary.
