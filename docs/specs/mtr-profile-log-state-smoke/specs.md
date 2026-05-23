# MTR Profile Log State Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.log_slow_filter`, `main.my_getopt_case_insensitive`, and
`main.log_state_bug33693`. This adds curated embedded baseline coverage for
slow-log variable parsing, case-insensitive startup option handling, and
general-log path state under the MyLite MTR profile.

## Non-Goals

- Broad logging, replication, binlog, or slow-query-log MTR coverage.
- Enabling daemon-only log rotation or external client behavior.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/log_slow_filter.test` verifies empty
  `log_slow_filter` configuration normalization and session assignment to the
  `ALL` value.
- `mariadb/mysql-test/main/my_getopt_case_insensitive.test` verifies that the
  startup option path sets `@@GLOBAL.slow_query_log`, covering the option
  parser's case-insensitive comparison behavior.
- `mariadb/mysql-test/main/log_state_bug33693.test` verifies that the derived
  general-log path is not based on the PID-file directory.
- All three tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Nearby candidates stay outside the curated list:
  `main.flush2` is non-embedded and exercises log rotation/binlog-adjacent
  state, `main.set_statement_profiling` and `main.variables_community` are
  skipped because profiling is disabled, and `main.contributors` fails
  intentionally because static SHOW information is disabled in the MyLite
  embedded profile.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected slow-log variable and log-path state behavior. This remains curated
MariaDB embedded baseline coverage, not broad server logging support and not
MyLite storage-routing evidence.

## Design

Add the three selected tests to `tools/mylite-mtr-harness`'s default curated
list near other profile and server metadata smoke cases. Do not modify
upstream test sources or expected result files.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile, not `libmylite` handles or diagnostics.

## Build, Size, And Dependencies

No production dependency or binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe main.log_slow_filter main.my_getopt_case_insensitive main.log_state_bug33693`
- `tools/mylite-mtr-harness run main.log_slow_filter main.my_getopt_case_insensitive main.log_state_bug33693`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the three selected log-state tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader logging tests need separate review because many upstream cases depend
  on non-embedded server lifecycle, binlog, replication, or log-rotation
  behavior that is outside the core embedded profile.
