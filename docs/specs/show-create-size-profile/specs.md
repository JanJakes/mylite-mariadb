# SHOW CREATE Size Profile

## Problem Statement

The aggressive MyLite minsize profile still links MariaDB's `SHOW CREATE`
runtime and CREATE-statement rendering helpers. Those helpers format
`SHOW CREATE TABLE`, `SHOW CREATE DATABASE`, `SHOW CREATE SERVER`, and
`SHOW CREATE TRIGGER` output, and the table renderer is also referenced by
binlog-oriented `CREATE TABLE ... LIKE` and `CREATE TABLE ... SELECT`
fallback paths.

MyLite's current size profile already disables binlog core, trigger runtime,
view runtime, stored-program runtime, SQL sequences, and foreign-server
metadata. `SHOW CREATE` remains useful for compatibility and debugging, but it
is not required for an embedded application to open a file, run SQL, and close
it. This slice measures an aggressive profile that rejects the runtime surface
and compiles out the linked formatting implementation.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/sql_yacc.yy` parses `SHOW CREATE DATABASE`,
  `SHOW CREATE TABLE`, `SHOW CREATE VIEW`, `SHOW CREATE SEQUENCE`,
  `SHOW CREATE SERVER`, `SHOW CREATE TRIGGER`, `SHOW CREATE EVENT`, and
  stored routine/package variants into distinct `SQLCOM_SHOW_CREATE*`
  commands.
- `vendor/mariadb/server/sql/sql_parse.cc` dispatches table/view/sequence
  forms through `mysqld_show_create()`, database through
  `show_create_db()` and `mysqld_show_create_db()`, server through
  `mysql_show_create_server()`, trigger through `show_create_trigger()`,
  event through `Events::show_create_event()`, and routine/package forms
  through `Sp_handler::sp_show_create_routine()`.
- `vendor/mariadb/server/sql/sql_show.cc` defines
  `mysqld_show_create_get_fields()`, `mysqld_show_create()`,
  `mysqld_show_create_db_get_fields()`, `mysqld_show_create_db()`,
  `mysql_show_create_server()`, `show_create_table()`,
  `show_create_table_ex()`, `show_create_sequence()`, `show_create_view()`,
  and `show_create_trigger()`.
- `vendor/mariadb/server/sql/sql_insert.cc:select_create::postlock()` calls
  `show_create_table()` only to produce row-binlog CREATE text for
  `CREATE TABLE ... SELECT`.
- `vendor/mariadb/server/sql/sql_table.cc:create_table_like()` calls
  `show_create_table()` only to produce binlog CREATE text for source tables
  that cannot be replicated as the original statement.
- The current minsize linked smoke still defines the table/database/server
  `SHOW CREATE` helpers and `show_create_table_ex()`.

## Scope

This slice may:

- add `MYLITE_DISABLE_SHOW_CREATE_RUNTIME`,
- enable it in `tools/build-mariadb-minsize.sh`,
- route all `SQLCOM_SHOW_CREATE*` commands to explicit unsupported diagnostics
  in the aggressive embedded profile,
- replace the linked `sql_show.cc` `SHOW CREATE` entry points with small
  unsupported or inert stubs, and
- add smoke assertions for unsupported `SHOW CREATE` diagnostics and absence
  of large linked formatter symbols.

## Non-Goals

This slice does not:

- remove parser syntax for `SHOW CREATE`,
- remove ordinary `SHOW TABLES`, `SHOW COLUMNS`, `SHOW VARIABLES`, or
  Information Schema queries,
- remove `append_identifier()` or `append_definer()` helpers used by retained
  DDL, event, CTE, and view code,
- change non-minsize MariaDB behavior,
- implement a MyLite-native schema dump API, or
- remove `CREATE TABLE`, `CREATE TABLE ... LIKE`, or `CREATE TABLE ... SELECT`
  execution.

## Proposed Design

Add a minsize option named `MYLITE_DISABLE_SHOW_CREATE_RUNTIME`.

When enabled, `sql_parse.cc` should reject all runtime `SHOW CREATE` command
variants with `ER_NOT_SUPPORTED_YET` and a message fragment containing
`SHOW CREATE`. This keeps SQL syntax parseable and yields a deliberate
compatibility reduction instead of an accidental linker failure.

In `sql_show.cc`, guard the heavy `SHOW CREATE` helpers with the same macro.
The replacement definitions should:

- keep `mysqld_show_create_db_get_fields()` as a tiny no-op because
  `sql_prepare.cc` still references it in the retained archive object;
- return unsupported from direct SQL result producers; and
- make `show_create_table()` / `show_create_table_ex()` return success without
  writing CREATE text when called from unreachable binlog-only branches.

The table renderer stubs are intentionally inert rather than erroring because
the only retained non-SQL references are binlog string-generation paths in a
build where binlog core is disabled. If a future profile re-enables binlog
core, this option should stay off or be revisited.

## Affected Subsystems

- SQL command dispatch in `sql_parse.cc`.
- `SHOW CREATE` result production and CREATE-statement formatting in
  `sql_show.cc`.
- Minsize CMake configuration.
- Open/close unsupported-profile smoke coverage.
- Binary-size documentation.

## DDL Metadata Routing Impact

No storage metadata is rerouted. The slice removes an introspection/runtime
surface from the aggressive size profile only. Core DDL execution and MyLite
catalog routing remain covered by existing storage and compatibility tests.

## Single-File And Embedded-Lifecycle Impact

No file-format change. The removed runtime does not create persistent files,
but rejecting it avoids retaining server-style metadata formatting for views,
triggers, sequences, foreign servers, and binlog-generated CREATE text.

## Public API Or File-Format Impact

No public `libmylite` C API change and no `.mylite` file-format change.

SQL compatibility impact: the aggressive minsize profile no longer supports
`SHOW CREATE ...`.

## Binary-Size Impact

Before this slice, the linked smoke defined these `SHOW CREATE` helpers:

| Symbol | Bytes |
| --- | ---: |
| `show_create_table_ex()` | 3,968 |
| `mysqld_show_create_get_fields()` | 1,836 |
| `mysql_show_create_server()` | 812 |
| `mysqld_show_create_db()` | 780 |
| `mysqld_show_create()` | 664 |
| `show_create_trigger()` | 252 |
| `view_store_options()` | 200 |
| `mysqld_show_create_db_get_fields()` | 168 |
| `show_create_table()` | 24 |

The final linked savings may exceed those symbol bodies slightly because
format strings and helper references can also be dropped, but this is not
expected to be a large win.

Measured after implementation against
`build/mariadb-minsize-no-prepared-api`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,489,666 | 25,389,516 | -100,150 |
| `sql_show.cc.o` | 474,880 | 380,360 | -94,520 |
| `sql_parse.cc.o` | 282,944 | 280,520 | -2,424 |
| `yy_mariadb.cc.o` | 1,246,128 | 1,246,240 | +112 |
| unstripped `mylite-open-close-smoke` | 6,503,344 | 6,486,024 | -17,320 |
| stripped `mylite-open-close-smoke` | 4,566,688 | 4,552,192 | -14,496 |
| `size` decimal for `mylite-open-close-smoke` | 4,790,812 | 4,774,460 | -16,352 |

The linked smoke no longer defines the heavy `show_create_table_ex()`,
`mysqld_show_create_get_fields()`, `mysqld_show_create()`,
`mysqld_show_create_db()`, `mysql_show_create_server()`,
`show_create_trigger()`, `show_create_view()`, or `show_create_sequence()`
runtime bodies. Tiny stored-program `SHOW CREATE` stubs remain from the
already-disabled stored-program runtime, and the test binary naturally keeps
its `SHOW CREATE` test-case strings.

## License, Trademark, And Dependency Impact

This is a GPL-2.0-only MariaDB-derived build-profile change. It adds no
dependency and changes no trademark-facing packaging.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh \
  tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

Measure:

- archive bytes and object deltas;
- unstripped and stripped linked smoke bytes;
- absence of linked heavy `SHOW CREATE` symbols; and
- ordinary DDL/storage smoke behavior.

## Acceptance Criteria

- The minsize build completes.
- `SHOW CREATE TABLE`, `SHOW CREATE DATABASE`, `SHOW CREATE SERVER`,
  `SHOW CREATE TRIGGER`, `SHOW CREATE EVENT`, and stored-routine/package
  variants report explicit unsupported diagnostics in the minsize profile.
- Existing storage and compatibility smokes still pass.
- Linked heavy `SHOW CREATE` formatter symbols are absent or reduced to tiny
  stubs.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-show-create \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh \
  tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The open/close report records explicit unsupported diagnostics for
`SHOW CREATE TABLE`, `SHOW CREATE DATABASE`, `SHOW CREATE SERVER`,
`SHOW CREATE TRIGGER`, `SHOW CREATE EVENT`, and
`SHOW CREATE PROCEDURE` / `PACKAGE` / `PACKAGE BODY`.

## Risks And Unresolved Questions

- `SHOW CREATE TABLE` is a useful compatibility and debugging feature. This
  should remain an aggressive size-profile experiment unless the final default
  prioritizes minimum binary size over SQL introspection.
- The inert `show_create_table()` binlog-path stub relies on the current
  minsize profile disabling binlog core. A profile that re-enables binlog
  replication must not combine that with this option without a fresh design.
- It is unknown before measurement whether section GC has already removed most
  indirect formatting dependencies.
