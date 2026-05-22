# MTR DDL Policy Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.mdev19198` and `main.deprecated_features`. This adds curated embedded
baseline coverage for lock-table behavior around `CREATE TABLE IF NOT EXISTS
... LIKE` and deprecated server syntax / removed compatibility-variable
rejection.

## Non-Goals

- Broad lock, metadata-lock, deprecated-syntax, or server-surface MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing upstream tests that depend on disabled native engines, missing
  server-only functions, host-file SQL I/O, sequence tables, XML/GIS helpers,
  or non-embedded client behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/mdev19198.test` covers `LOCK TABLES` interaction
  with `CREATE TABLE IF NOT EXISTS ... LIKE`, including a successful no-op
  under a write/read lock set and `ER_TABLE_NOT_LOCKED_FOR_WRITE` under
  read/read locks.
- `mariadb/mysql-test/main/deprecated_features.test` covers rejection of
  removed/deprecated compatibility surfaces: `log_bin_trust_routine_creators`,
  `table_type`, `BACKUP TABLE`, `RESTORE TABLE`, `SHOW PLUGIN`,
  `LOAD ... FROM MASTER`, `SHOW INNODB STATUS`, old `TYPE=MyISAM` table
  syntax, `SHOW TABLE TYPES`, and `SHOW MUTEX STATUS`.
- Both selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby candidates stay outside this slice:
  - `main.func_if` and `main.func_set` reach explicit disabled native MyISAM
    sections.
  - `main.func_isnull` reaches missing named-lock helper functions.
  - `main.func_math` reaches a trimmed GIS helper.
  - `main.func_str`, `main.subselect_no_exists_to_in`,
    `main.subselect_no_opts`, and `main.subselect_no_scache` require the
    sequence engine disabled in the current smoke profile.
  - `main.func_json` requires disabled InnoDB startup options.
  - `main.group_by_null` reaches the trimmed XML helper path.
  - `main.overflow` depends on embedded session-count behavior that does not
    normalize cleanly in the current profile.
  - `main.delimiter_command_case_sensitivity` and `main.mdev-31636` are
    non-embedded tests and are skipped by MTR.
  - `main.subselect3` reaches unsupported host-file SQL output.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected lock-table DDL behavior and deprecated server syntax rejection in
addition to existing bootstrap, scalar-expression, data-type, temporal,
parser, DDL metadata, optimizer, prepared-statement, diagnostic, charset,
collation, JSON, crypto, and aggregate smoke coverage. This remains curated
MariaDB embedded baseline coverage, not broad MTR-scale comparison and not
MyLite storage-routing evidence.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Do not modify upstream MariaDB MTR test files for this slice.
- Keep broader lock and deprecated-surface suites outside the list until their
  disabled-engine, sequence, unsupported-function, file-I/O, and non-embedded
  assumptions are reviewed separately.

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
- `tools/mylite-mtr-harness run main.mdev19198 main.deprecated_features`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.mdev19198` and
  `main.deprecated_features`.
- Both selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test-source normalization is needed for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This does not cover broad metadata-lock, table-lock, deprecated-option, or
  server-management behavior.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing DDL or SQL-lock policy behavior.
