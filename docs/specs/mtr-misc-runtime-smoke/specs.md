# MTR Misc Runtime Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.opt_trace_default`, `main.odbc`, `main.show_row_order-9226`,
`main.system_mysql_db_refs`, `main.long_tmpdir`, and `main.renamedb`.
This adds curated embedded baseline coverage for optimizer-trace defaults,
ODBC compatibility syntax, SHOW column ordering, system `mysql` table reference
integrity, long temporary directory view handling, and deprecated rename
database diagnostics.

## Non-Goals

- Broad misc, SHOW, metadata, optimizer-trace, or database-upgrade MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Re-enabling native MyISAM/InnoDB, XML, GIS, sequence, metadata-lock,
  status-counter, or server-only runtime surfaces.
- Normalizing upstream expected-result files for storage-engine or status
  counter drift.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/opt_trace_default.test` checks that
  `optimizer_trace` keeps its compile-time default of `enabled=off`.
- `mariadb/mysql-test/main/odbc.test` covers ODBC escape syntax,
  `sql_auto_is_null`, autoincrement lookup behavior, and `LAST_INSERT_ID()`
  preservation.
- `mariadb/mysql-test/main/show_row_order-9226.test` covers MDEV-9226 column
  ordering in `SHOW COLUMNS` for tables with large `ENUM` definitions.
- `mariadb/mysql-test/main/system_mysql_db_refs.test` copies core `mysql.*`
  grant tables and verifies common column widths/reference compatibility.
- `mariadb/mysql-test/main/long_tmpdir.test` covers a long-temporary-directory
  regression using a simple view over `information_schema.tables`.
- `mariadb/mysql-test/main/renamedb.test` covers deprecated `RENAME DATABASE`
  parse rejection and `ALTER DATABASE ... UPGRADE DATA DIRECTORY NAME`
  diagnostics.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed candidates intentionally left out of this slice:
  - `main.func_int`, `main.func_isnull`, `main.func_hybrid_type`,
    `main.func_group`, `main.func_date_add`, `main.func_time`,
    `main.type_temporal_mysql56`, `main.type_temporal_mariadb53`, and
    `main.mysql57_virtual` require disabled native engines or server functions.
  - `main.func_misc`, `main.func_gconcat`, and `main.mysqltest_ps` are skipped
    under the smoke profile and therefore provide no pass-gated coverage.
  - `main.group_by_null`, `main.mdev-35765`, and
    `main.show_function_with_pad_char_to_full_length` hit trimmed XML, GIS, or
    routine metadata surfaces.
  - `main.function_defaults` reaches SQL host-file output that the profile
    intentionally disables.
  - `main.select_safe`, `main.order_by_sortkey`, `main.win_i_s`,
    `main.type_varchar_mysql41`, and `main.stat_tables_missing` are sensitive
    to profile-specific plan, status, or default-engine output and need
    separate normalization review.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected miscellaneous runtime, metadata, ODBC, optimizer-trace default, SHOW
row-order, system-table reference, long-tmpdir, and deprecated rename-database
diagnostics. This remains curated MariaDB embedded baseline coverage, not broad
MTR-scale comparison and not MyLite storage-routing evidence.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Do not modify upstream MariaDB test files.
- Keep skipped, disabled-engine, unsupported-surface, and profile-drift
  candidates outside the list.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.opt_trace_default main.odbc main.show_row_order-9226 main.system_mysql_db_refs main.long_tmpdir main.renamedb`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected miscellaneous runtime tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Verification Results

- `tools/mylite-mtr-harness list`: 190 curated tests.
- `tools/mylite-mtr-harness run main.opt_trace_default main.odbc main.show_row_order-9226 main.system_mysql_db_refs main.long_tmpdir main.renamedb`: all 6 selected tests passed.
- `tools/mylite-mtr-harness run`: 8 MyLite tests and 182 upstream `main`
  tests passed.
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`

## Risks And Open Questions

- Broader misc, optimizer, metadata, SHOW, and function suites need separate
  disabled-engine, trimmed-surface, and profile-output normalization review.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing query behavior.
