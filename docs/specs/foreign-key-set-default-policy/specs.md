# Foreign-Key SET DEFAULT Policy

## Goal

Make MyLite's `SET DEFAULT` foreign-key policy explicit. MyLite should reject
`ON DELETE SET DEFAULT` and `ON UPDATE SET DEFAULT` before catalog publication
instead of treating them as roadmap work, because the selected MariaDB/InnoDB
base does not implement those actions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `SET DEFAULT` into
  `FK_OPTION_SET_DEFAULT`.
- `mariadb/sql/table.h` includes `FK_OPTION_SET_DEFAULT` in
  `enum_fk_option`, and `mariadb/sql/table.cc:fk_option_name()` can print the
  token.
- `mariadb/storage/innobase/include/dict0mem.h:dict_foreign_t` has action
  flags for `DELETE_CASCADE`, `DELETE_SET_NULL`, `UPDATE_CASCADE`,
  `UPDATE_SET_NULL`, `DELETE_NO_ACTION`, and `UPDATE_NO_ACTION`; it has no
  `SET DEFAULT` action flag.
- `mariadb/storage/innobase/handler/ha_innodb.cc` leaves both
  `FK_OPTION_SET_DEFAULT` switch arms as `TODO: MDEV-10393 Foreign keys SET
  DEFAULT action`.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_foreign_key_actions_supported()`
  rejects any requested action that is neither restrict-like, `SET NULL`, nor
  `CASCADE`.

Official MariaDB documentation lists `SET DEFAULT` in the grammar but also
documents it under foreign-key limitations as unsupported:

- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>
- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>

## Non-Goals

- Implementing non-MariaDB `SET DEFAULT` action semantics.
- Reinterpreting `SET DEFAULT` as `RESTRICT`, `NO ACTION`, or `SET NULL`.
- Changing MyLite's storage foreign-key metadata enum; it can keep the value
  for defensive serialization and future import tooling.
- Broadening cyclic cascade, transaction isolation, or multi-table action
  matrices.

## Compatibility Impact

`SET DEFAULT` should be documented as deliberately unsupported, not planned.
Applications that emit `SET DEFAULT` FK clauses get an explicit failure before
MyLite publishes table or FK catalog metadata. This is stricter than accepting
the syntax and silently doing no default assignment, but matches MariaDB's
documented unsupported status and avoids a misleading compatibility claim.

## Design

Keep the existing MyLite handler action gate:

1. classify update and delete actions independently;
2. accept restrict-like, `SET NULL`, and `CASCADE` actions only when their
   table-shape requirements pass;
3. reject `SET DEFAULT` for direct `CREATE TABLE`, prepared `CREATE TABLE`,
   and copy `ALTER TABLE ... ADD FOREIGN KEY`;
4. verify failed create paths leave no catalog table metadata and failed alter
   paths leave no FK metadata on the existing table.

Docs should remove `SET DEFAULT` from planned roadmap wording and describe it
as an explicit unsupported policy.

## File Lifecycle

No file-format or companion-file change is introduced. Failed `SET DEFAULT`
DDL must not create table records, FK records, durable sidecars, or retained
temporary schema metadata.

## Embedded Lifecycle And API

No public API change is introduced. Direct and prepared SQL execution should
surface the existing MyLite/MariaDB error diagnostics for unsupported FK DDL.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for direct `CREATE TABLE` rejection with
  `ON DELETE SET DEFAULT`.
- Add direct `CREATE TABLE` rejection with `ON UPDATE SET DEFAULT`.
- Add prepared `CREATE TABLE` rejection with both actions.
- Add copy `ALTER TABLE ... ADD FOREIGN KEY` rejection for both delete and
  update `SET DEFAULT`.
- Assert failed create paths leave no table metadata, failed alter paths leave
  no FK metadata, and close/reopen keeps the catalog clean.
- Run `git diff --check`, the focused storage-engine smoke binary,
  `ctest --preset storage-smoke-dev`, and `ctest --preset dev`.

## Acceptance Criteria

- `SET DEFAULT` no longer appears as planned roadmap work.
- Compatibility docs state the explicit unsupported policy.
- Direct and prepared DDL rejection paths are covered.
- Existing supported FK actions still pass their storage-smoke coverage.

## Risks And Open Questions

MariaDB source currently accepts the syntax in some paths while not
implementing default assignment. If a future MariaDB base implements
MDEV-10393, this policy should be revisited against that base's source and
official documentation.
