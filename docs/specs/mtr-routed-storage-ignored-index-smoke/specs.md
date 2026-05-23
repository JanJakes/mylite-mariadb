# MTR routed storage ignored index smoke

## Problem

MyLite preserves MariaDB ignored-index metadata for supported routed secondary
indexes, and first-party storage smoke covers close/reopen persistence. The raw
storage-routed MTR list does not yet exercise ignored-index metadata and forced
hint behavior through MariaDB's embedded test runner.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documents ignored indexes as optimizer-hidden indexes and exposes
  their state through `SHOW INDEX` and `INFORMATION_SCHEMA.STATISTICS`.
- `mariadb/sql/sql_yacc.yy` parses `IGNORED` / `NOT IGNORED` in index
  definitions and `ALTER TABLE ... ALTER INDEX ... [NOT] IGNORED`.
- `mariadb/sql/sql_table.cc` copies create-time ignored flags into `KEY`
  metadata and applies alter-time ignorability changes during table rebuilds.
- `mariadb/sql/unireg.cc` serializes ignored-index flags into the binary table
  definition image, and `mariadb/sql/table.cc` restores them into
  `TABLE_SHARE::ignored_indexes`.
- `mariadb/sql/table.cc` removes ignored indexes from optimizer-usable indexes,
  which makes `FORCE INDEX` against an ignored index fail with
  `ER_KEY_DOES_NOT_EXISTS`.
- `mariadb/sql/sql_show.cc` exposes ignored-index state in the `IGNORED`
  column.
- MyLite stores and rediscovers MariaDB's binary table definition image through
  the MyLite catalog for routed tables; ignored-index state should therefore
  round-trip without extra MyLite file-format changes.

## Design

Add `mylite.routed_storage_ignored_indexes` to the storage MTR list. The test
runs with a primary `.mylite` file and enforced MyLite storage. It creates a
routed table with one ignored secondary index and one visible secondary index,
then verifies representative raw ignored-index behavior:

- `INFORMATION_SCHEMA.STATISTICS.IGNORED` reports `YES` / `NO`;
- a forced hint against the visible index works;
- forced hints against ignored indexes fail;
- `CREATE INDEX ... IGNORED ALGORITHM=COPY` publishes ignored metadata; and
- `ALTER TABLE ... ALTER INDEX ... IGNORED` and `... NOT IGNORED` toggle
  optimizer visibility through the supported copy-ALTER path.

The test also records `SHOW CREATE TABLE` output and sidecar absence.

## Scope

This is test and documentation work only. It does not add MySQL
visible/invisible syntax, primary-key ignorability, unsupported index-class
ignorability, online/in-place ignored-index ALTER, close/reopen MTR coverage,
or new physical index behavior.

## Compatibility Impact

The storage MTR runner gains direct evidence that raw embedded MyLite storage
sessions preserve MariaDB ignored-index metadata and hint rejection behavior for
supported secondary indexes. Broader ignored-index persistence and close/reopen
coverage remains owned by first-party storage smoke.

## Storage And Lifecycle Impact

Ignored-index metadata remains part of the routed table definition stored in
the primary `.mylite` catalog. The raw MTR test must not create forbidden
durable sidecars.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage
  mylite.routed_storage_ignored_indexes`
- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_ignored_indexes`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- Initial ignored and not-ignored secondary indexes expose the expected
  `IGNORED` metadata.
- Forced hints against ignored indexes fail with `ER_KEY_DOES_NOT_EXISTS`.
- Forced hints against visible supported indexes work.
- Standalone ignored index creation publishes ignored metadata.
- Copy `ALTER INDEX ... IGNORED` and `... NOT IGNORED` toggle metadata and hint
  behavior.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed ignored-index
  coverage without claiming unsupported index-class or online ALTER support.

## Risks

Optimizer plan choice can be data-dependent, so the test uses metadata
assertions and forced-hint success/failure rather than plan-shape expectations.
