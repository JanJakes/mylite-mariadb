# MTR Aria Optimizer Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.range_aria_dbt3` and
`main.subselect_sj_aria`. This adds upstream embedded baseline coverage for
Aria-backed range optimizer regressions over DBT3-shaped data and an
Aria-specific semijoin regression.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level range, semijoin, or Aria implementation.
- Enabling native MyISAM, InnoDB, partitioning, Sequence-engine, or debug-only
  test surfaces.
- Changing SQL optimizer behavior, storage behavior, metadata routing, or file
  format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/range_aria_dbt3.test` sets
  `default_storage_engine=Aria`, loads DBT3-scale fixture tables through
  `include/dbt3_s001.inc`, and covers range-access regressions including index
  condition interaction, compound `OR` ranges, and join planning.
- `mariadb/mysql-test/main/subselect_sj_aria.test` creates explicit Aria
  tables and covers a semijoin regression that asserted inside Aria key access.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.range_aria_dbt3 main.subselect_sj_aria`
  passed without test patching.
- Rejected nearby candidates in this pass were left out because they reached
  disabled server-oriented or profile-mismatched surfaces: `main.having`
  includes multiple native MyISAM sections and a `mysql.help_topic` system-table
  section, `main.ctype_dec8` reaches Oracle SQL mode, and
  `main.ctype_utf16_def` expects a full-server `ft_stopword_file` default.

## Design

- Add `main.subselect_sj_aria` beside existing selected subquery smoke tests.
- Add `main.range_aria_dbt3` beside existing optimizer and rowid-filter smoke
  tests.
- Do not patch upstream MariaDB test files; both selected tests already match
  the smoke profile's available Aria engine.
- Scope docs to selected Aria range, semijoin, and rowid-filter optimizer
  behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected Aria-backed optimizer behavior. This is MariaDB embedded compatibility
evidence, not a new MyLite routed-storage claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The tests run inside the MTR vardir
with MariaDB's Aria files.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change. The tests validate MariaDB embedded optimizer
behavior under the smoke profile's Aria-backed MTR tables.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.range_aria_dbt3 main.subselect_sj_aria`
- `tools/mylite-mtr-harness run main.range_aria_dbt3 main.subselect_sj_aria`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- Both selected tests report MTR passes under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected Aria optimizer behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.range_aria_dbt3 main.subselect_sj_aria`:
  passed.
- `tools/mylite-mtr-harness run main.range_aria_dbt3 main.subselect_sj_aria`:
  passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 162 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `163`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

These tests are optimizer-plan sensitive and use Aria rather than MyLite
storage. Future MyLite routed-storage optimizer claims need storage-smoke
coverage over MyLite handler tables instead of relying on this MariaDB embedded
baseline.
