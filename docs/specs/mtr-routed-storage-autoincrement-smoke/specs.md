# MTR routed storage autoincrement smoke

## Problem

MyLite has extensive first-party storage coverage for durable and volatile
autoincrement behavior, but the raw embedded storage-routed MTR list does not
yet include a focused autoincrement lifecycle test. The storage MTR smoke
runner should prove representative generated values, explicit high-value
advancement, rollback gap preservation, session offset/increment handling, and
truncate reset through the same embedded MariaDB path used by upstream MTR
tests.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::update_auto_increment()` owns generated
  autoincrement value assignment, statement interval reservation, explicit
  value handling, and session `auto_increment_offset` /
  `auto_increment_increment` rounding.
- `mariadb/sql/handler.cc::compute_next_insert_id()` computes the next
  generated value under session offset/increment settings.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::get_auto_increment()`
  reads MyLite table-local autoincrement state, reserves durable generated
  intervals for first-key autoincrement tables, and preserves generated gaps
  across rollback.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` advances
  durable autoincrement state after generated or successful explicit
  high-value row writes and after duplicate/FK checks.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::reset_auto_increment()`
  maps `TRUNCATE TABLE` / reset paths to table-local MyLite autoincrement
  state.

## Design

Add `mylite.routed_storage_autoincrement` to the storage MTR list. The test
runs with one primary `.mylite` file, enforces MyLite storage, and creates
parallel explicit `ENGINE=InnoDB` and `ENGINE=MYLITE` tables. It verifies:

- multi-row generated inserts return the first generated `LAST_INSERT_ID()`;
- successful explicit high autoincrement values advance the next generated
  value;
- generated values reserved inside rolled-back transactions remain gaps;
- session `auto_increment_offset` / `auto_increment_increment` are honored; and
- `TRUNCATE TABLE` resets table-local generated values.

The test keeps the SQL shape compact because requested-engine metadata is
already covered by `mylite.routed_storage_engines`.

## Scope

This is test and documentation work only. It does not add new autoincrement
semantics, grouped later-in-key coverage, exhaustive integer-width matrices,
trigger/view behavior, or new public APIs.

## Compatibility Impact

The storage MTR runner gains direct evidence that representative
MySQL/MariaDB-oriented autoincrement behavior survives MyLite storage routing
when applications request `ENGINE=InnoDB` and when they request `ENGINE=MYLITE`
directly.

## Storage And Lifecycle Impact

All durable table rows, index entries, and autoincrement state remain in the
primary `.mylite` file. The sidecar assertion rejects native engine sidecars
for the MyLite-owned schema before and after the autoincrement operations.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage mylite.routed_storage_autoincrement`
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_autoincrement`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new storage MTR autoincrement test passes.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed autoincrement MTR
  coverage.

## Risks

The test is representative, not exhaustive. Broader grouped autoincrement,
trigger/view, expression-error, and integer-width matrices remain covered by
first-party storage tests or planned follow-up slices.
