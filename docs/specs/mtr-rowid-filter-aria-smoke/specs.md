# MTR Aria Rowid-Filter Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.rowid_filter_aria`. This
adds upstream embedded baseline coverage for rowid-filter optimizer selection,
`EXPLAIN`, `EXPLAIN FORMAT=JSON`, `ANALYZE`, and result stability over Aria
tables with DBT3-shaped data.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level rowid-filter implementation.
- Enabling native MyISAM, InnoDB, partitioning, or Sequence-engine support.
- Changing SQL optimizer behavior, storage behavior, metadata routing, or file
  format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/rowid_filter_aria.test` sets
  `DEFAULT_STORAGE_ENGINE='Aria'` and sources `rowid_filter.test`.
- `mariadb/mysql-test/main/rowid_filter.test` loads DBT3-scale fixture data
  through `include/dbt3_s001.inc`, creates secondary indexes, populates
  statistics, and compares plans and results with `optimizer_switch`
  `rowid_filter=on` and `rowid_filter=off`.
- The test exercises `SHOW CREATE TABLE`, `EXPLAIN`, `EXPLAIN FORMAT=JSON`,
  `ANALYZE`, `ANALYZE FORMAT=JSON`, joins, CHECK constraints, and a view under
  the smoke profile's enabled embedded view runtime.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.rowid_filter_aria` passed without test
  patching. Related probes that failed in this pass were rejected for
  profile-mismatched reasons: embedded session accounting in `main.overflow`,
  expected native system-table differences in `main.1st`, and disabled
  host-file export in `main.subselect3`.

## Design

- Add `main.rowid_filter_aria` to the curated MTR smoke list near existing
  optimizer and EXPLAIN coverage.
- Do not patch upstream MariaDB test files; the Aria variant already matches
  the smoke profile's available persistent engine.
- Scope docs to selected Aria rowid-filter optimizer behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected rowid-filter optimizer behavior on Aria tables. This is MariaDB
embedded compatibility evidence, not a new MyLite routed-storage claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR vardir
with MariaDB's Aria files.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change. The test validates MariaDB embedded optimizer behavior
under the smoke profile's Aria-backed MTR tables.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.rowid_filter_aria`
- `tools/mylite-mtr-harness run main.rowid_filter_aria`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.rowid_filter_aria` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected Aria rowid-filter optimizer behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.rowid_filter_aria`: passed.
- `tools/mylite-mtr-harness run main.rowid_filter_aria`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 160 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `161`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test is optimizer-plan sensitive and uses Aria rather than MyLite storage.
Future MyLite routed-storage optimizer claims need storage-smoke coverage over
MyLite handler tables instead of relying on this MariaDB embedded baseline.
