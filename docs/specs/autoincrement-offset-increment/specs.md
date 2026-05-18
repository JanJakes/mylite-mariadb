# Autoincrement Offset And Increment Coverage

## Goal

Cover MyLite-routed `AUTO_INCREMENT` allocation under non-default session
`auto_increment_offset` and `auto_increment_increment` values for both
table-local first-key allocation and grouped later-in-key allocation.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:Sys_auto_increment_increment` and
  `Sys_auto_increment_offset` define session-scoped variables for generated
  value spacing.
- `mariadb/sql/handler.cc:compute_next_insert_id()` rounds generated ids to
  values of the form `auto_increment_offset + N * auto_increment_increment`.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` passes the session
  offset and increment into the storage-engine `get_auto_increment()` hook and
  rounds again for engines that do not.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` already
  uses `mylite_first_auto_increment_value()` for table-local allocation, and
  the grouped-prefix slice now feeds grouped prefix maxima through the same
  helper.

## Scope

- Add storage-engine smoke coverage for a first-key routed table with
  `auto_increment_offset=2` and `auto_increment_increment=3`.
- Add storage-engine smoke coverage for a grouped later-in-key routed table
  under the same session settings.
- Cover generated inserts, explicit high values, per-prefix independence, and
  close/reopen persistence.
- Reset the session variables to defaults before closing each embedded handle.

## Non-Goals

- Exhaustive matrices for every legal offset/increment pair.
- Multi-row post-explicit allocation under a different offset/increment pair,
  which is covered by the separate
  `autoincrement-offset-increment-multi-row` slice.
- Offset values larger than increment.
- Negative explicit values, overflow, or integer-width edge cases.
- Transaction-aware rollback of consumed generated values.
- New storage format or public API behavior.

## Compatibility Impact

The slice moves representative session offset/increment coverage from planned
to partial for the routed storage subset. It does not claim full exhaustive
matrix coverage.

## Design

No new storage path is required. The tests exercise the existing MariaDB
handler flow:

1. set the session variables on the embedded connection;
2. create first-key and grouped-prefix routed tables;
3. insert generated rows and assert ids follow the configured sequence;
4. insert explicit high values and assert later generated ids round to the next
   configured value;
5. reopen the database and repeat one generated insert for each table shape.

For grouped prefixes, expected values are computed per prefix. With offset `2`
and increment `3`, a new prefix starts at `2`; after explicit value `20`, the
next generated value in that prefix is `23`.

## File Lifecycle

No file-format or companion-file change is introduced. First-key allocation
continues to persist table-local autoincrement state in the primary `.mylite`
file. Grouped allocation continues to derive values from live rows in the
primary file.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is exposed through
ordinary SQL session variable assignment and inserts.

## Storage-Engine Routing

The coverage applies to MyLite-routed durable tables. The grouped case uses an
explicit MyISAM request because MariaDB's grouped source model comes from
MyISAM/Aria, while the MyLite handler implements the physical behavior.

## Build, Size, And Dependencies

No dependency or intended binary-size-profile change is introduced. This is a
test and documentation slice unless the new tests expose a bug.

## Test Plan

- Extend `mylite_embedded_storage_engine_test` with first-key and grouped
  offset/increment cases.
- Run `git diff --check`, the focused storage-engine smoke binary,
  `ctest --preset storage-smoke-dev`, and `ctest --preset dev`.

## Acceptance Criteria

- First-key generated ids honor `auto_increment_offset=2` and
  `auto_increment_increment=3`.
- Grouped generated ids honor the same settings independently per prefix.
- Explicit high values advance to the next matching configured value.
- Reopen preserves the expected next values.
- Compatibility and roadmap docs distinguish representative coverage from
  exhaustive matrices.

## Risks And Open Questions

- MyLite still needs broader offset/increment matrices across integer widths,
  temporary/volatile rows, and overflow boundaries before claiming exhaustive
  compatibility.
- Multi-row post-explicit first-key and grouped-prefix allocation is covered by
  the separate `autoincrement-offset-increment-multi-row` slice.
