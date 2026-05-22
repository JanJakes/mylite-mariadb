# MTR Charset Create Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.ctype_create`. This adds
upstream embedded baseline coverage for charset and collation propagation
through `CREATE DATABASE`, `ALTER DATABASE`, `CREATE TABLE`, and `ALTER TABLE
... CONVERT TO CHARACTER SET` forms.

## Non-Goals

- Broad charset DDL MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Changing MyLite SQL behavior, metadata routing, storage behavior, or file
  format.
- Promoting charset suites that depend on disabled native MyISAM/InnoDB,
  Sequence, dynamic-column, host-file, or benchmark surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ctype_create.test` checks server-level default
  charset inheritance for databases, database-level charset inheritance for
  tables, database option hash updates after `ALTER DATABASE`, option cleanup
  after `DROP DATABASE`, conflict diagnostics for duplicate charset/collation
  declarations, and MDEV-7387/MDEV-28644 `ALTER TABLE` charset conversion
  declaration ordering.
- The only observed probe drift is the MTR smoke profile's Aria default engine
  text in `SHOW CREATE TABLE` output. Upstream expected results contain
  `ENGINE=MyISAM`, while the profile reports
  `ENGINE=Aria ... PAGE_CHECKSUM=1`.
- Existing admitted MTR tests use
  `--replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""` for this
  profile-specific default-engine output.
- Rejected nearby probes remain outside this slice:
  - `main.ctype_binary` reaches an omitted `BENCHMARK()` path.
  - `main.ctype_filename` adds disabled native-MyISAM warning drift.
  - `main.ctype_utf8` and `main.type_timestamp_hires` require disabled InnoDB
    information-schema option registration during embedded bootstrap.
  - `main.ctype_utf16`, `main.ctype_utf32`, `main.ctype_nopad_8bit`, and
    `main.ctype_cp1251` reach explicit native-MyISAM sections.
  - `main.ctype_collate` reaches disabled dynamic-column runtime.

## Design

- Add standard Aria/MyISAM `SHOW CREATE TABLE` result normalization before each
  table metadata assertion in `ctype_create.test`.
- Add `main.ctype_create` to the curated MTR smoke list near the existing
  charset/collation tests.
- Keep docs explicit that this is curated MariaDB embedded baseline evidence,
  not storage-routing coverage.

## Compatibility Impact

The curated MTR smoke runner gains upstream coverage for charset and collation
inheritance plus charset-conversion DDL diagnostics. No MyLite runtime behavior
changes.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs in the MTR smoke
vardir.

## Public API And File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

No routing change. The test runs with the MTR smoke profile's embedded runtime
and default Aria engine setting.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.ctype_create`
- `tools/mylite-mtr-harness run main.ctype_create`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.ctype_create` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected charset CREATE/ALTER behavior.

## Risks And Follow-Up

The broader charset MTR surface still contains many disabled-engine and
trimmed-runtime dependencies. Larger promotion batches should continue to use
probe evidence rather than normalizing disabled product surfaces into the smoke
list.
