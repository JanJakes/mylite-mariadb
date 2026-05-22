# MTR Aria ALTER Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with
`main.alter_table_mdev539_maria` and `main.alter_table_upgrade_aria`. This adds
upstream embedded baseline coverage for Aria primary/unique index rebuild
behavior over DBT3-shaped tables and Aria table-upgrade ALTER/CHECK/REPAIR
paths.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level Aria, table-upgrade, CHECK TABLE, REPAIR TABLE,
  or native datadir sidecar behavior.
- Enabling native MyISAM, InnoDB, partitioning, or Sequence-engine support.
- Changing MyLite SQL behavior, metadata routing, storage behavior, or file
  format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/alter_table_mdev539_maria.test` sets the default
  storage engine to Aria and sources `include/alter_table_mdev539.inc`, which
  loads DBT3-scale fixture tables and covers fast primary/unique index rebuilds,
  duplicate-key failure, `ALTER IGNORE`, and `SHOW CREATE TABLE` stability.
- `mariadb/mysql-test/main/alter_table_upgrade_aria.test` uses
  `have_aria.inc`, points the shared upgrade include at Aria fixture files, and
  covers `CHECK TABLE ... FOR UPGRADE`, `ALTER TABLE ... ALGORITHM=INSTANT`,
  `ALTER TABLE ... ALGORITHM=NOCOPY`, `REPAIR TABLE`, and `ALTER TABLE FORCE`.
- `mariadb/mysql-test/main/alter_table_upgrade_mdev29481_myisam_aria.inc`
  copies upstream `.frm`, `.MAD`, and `.MAI` files into the MTR datadir. That is
  acceptable only as MariaDB embedded baseline evidence inside the MTR vardir;
  it is not MyLite single-file behavior.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.alter_table_mdev539_maria` passed.
  `tools/mylite-mtr-harness probe main.alter_table_upgrade_aria` passed.
- A nearby probe, `main.variables_community`, was left out because it
  self-skips on `have_profiling`, which is disabled in the trimmed profile.

## Design

- Add both Aria ALTER tests near existing DDL/index MTR smoke tests.
- Do not patch upstream MariaDB test files; both selected tests already match
  the smoke profile's available Aria engine.
- Scope docs to selected Aria ALTER/index-upgrade behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected Aria ALTER and index-upgrade behavior. This is MariaDB embedded
compatibility evidence, not a new MyLite routed-storage or single-file claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The tests run inside the MTR vardir
and may create MariaDB-owned Aria and `.frm` files there.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change. The tests validate MariaDB embedded behavior under
the smoke profile's Aria-backed MTR tables.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.alter_table_mdev539_maria`
- `tools/mylite-mtr-harness probe main.alter_table_upgrade_aria`
- `tools/mylite-mtr-harness run main.alter_table_mdev539_maria main.alter_table_upgrade_aria`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- Both selected tests report MTR passes under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected Aria ALTER/index-upgrade behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.alter_table_mdev539_maria`: passed.
- `tools/mylite-mtr-harness probe main.alter_table_upgrade_aria`: passed.
- `tools/mylite-mtr-harness run main.alter_table_mdev539_maria main.alter_table_upgrade_aria`:
  passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 164 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `165`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

`main.alter_table_upgrade_aria` intentionally exercises MariaDB datadir upgrade
paths by copying old-format table files into the MTR vardir. MyLite's final
single-file metadata and storage lifecycle still need first-party routed-storage
tests; this MTR slice only preserves upstream embedded behavior awareness.
