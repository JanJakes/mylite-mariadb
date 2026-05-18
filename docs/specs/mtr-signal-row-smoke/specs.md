# MTR Signal And Row Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.signal`, `main.signal_demo1`, `main.signal_demo3`,
`main.signal_sqlmode`, and `main.row`. This adds curated embedded baseline
coverage for `SIGNAL` / `RESIGNAL` syntax and diagnostics, SQL-mode-sensitive
condition item truncation, stored-program/trigger SIGNAL demonstrations, and
row-constructor comparison behavior.

## Non-Goals

- Broad stored-program, trigger, diagnostics-area, or row-constructor MTR
  coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Admitting nearby diagnostics tests that require disabled native engines,
  timing-dependent execution, debug-only features, disabled server utility
  functions, `LOAD DATA`, or missing system log tables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/signal.test` covers SIGNAL and RESIGNAL syntax,
  reserved/non-reserved diagnostics identifiers, condition declarations,
  diagnostics item assignment, handler behavior, and stored routine error
  propagation.
- `mariadb/mysql-test/main/signal_demo1.test` demonstrates SIGNAL-based
  integrity checks across stored procedures and triggers.
- `mariadb/mysql-test/main/signal_demo3.test` demonstrates RESIGNAL stack
  reporting and `SHOW WARNINGS` rows under different `max_error_count`
  settings.
- `mariadb/mysql-test/main/signal_sqlmode.test` covers SQL-mode-sensitive
  truncation and error behavior for SIGNAL condition items.
- `mariadb/mysql-test/main/row.test` covers row constructors in `IN`
  predicates, equality and ordering comparisons, NULL-safe equality, nested row
  expression errors, row equality optimizations, and table-backed comparisons.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby candidates remain outside this slice:
  - `main.signal_code` requires a debug build.
  - `main.signal_demo2` depends on disabled `GET_LOCK()`.
  - `main.get_diagnostics` and `main.strict` require disabled InnoDB startup
    surfaces.
  - `main.warnings` reaches host-file `LOAD DATA`.
  - `main.max_statement_time` is disabled upstream as timing-dependent.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior in addition
to existing SQL execution, expression, prepared-statement, charset, and type
coverage. This remains curated MariaDB embedded baseline coverage, not broad
MTR-scale comparison and not MyLite storage-routing evidence.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Do not modify upstream MariaDB test files for this slice.
- Keep nearby diagnostics tests with disabled engines, disabled server
  functions, timing dependencies, or host-file I/O outside the curated list.

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
- `tools/mylite-mtr-harness run main.signal main.signal_demo1 main.signal_demo3 main.signal_sqlmode main.row`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The selected SIGNAL tests exercise MariaDB's stored-program and trigger
  runtime in the opt-in MTR profile. They do not change MyLite's default
  product policy for persistent routines or triggers.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing behavior.
