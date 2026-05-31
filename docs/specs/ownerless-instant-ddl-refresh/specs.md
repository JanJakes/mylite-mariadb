# Ownerless Instant DDL Refresh

## Problem

Ownerless DDL coverage already proves peer refresh for create, rename,
truncate, foreign keys, generated columns, `CREATE TABLE ... LIKE`,
`CREATE TABLE ... SELECT`, and online index alteration. The remaining DDL gap
still calls out broader online DDL classes. MariaDB's InnoDB instant column
path changes dictionary metadata without rebuilding the table, so an already
open ownerless peer must observe the new column layout, defaults, dropped-column
metadata, and reordered columns before it continues with reads or writes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc` defines
  `INNOBASE_ALTER_INSTANT` and the instant column metadata helpers around
  `dict_table_t::prepare_instant()`, `dict_table_t::instant_column()`, and
  `innobase_instant_try()`.
- `handler0alter.cc:1687-1936` checks whether ADD, DROP, and stored-column
  reorder operations can avoid rebuild through `instant_alter_column_possible()`.
- `handler0alter.cc:2200-2360` reports `HA_ALTER_INPLACE_INSTANT` from
  `ha_innobase::check_if_supported_inplace_alter()` when InnoDB can perform the
  alter through the instant path.
- `handler0alter.cc:10900-10935` commits instant changes through
  `innobase_instant_try()` instead of the normal table rebuild path.
- `mariadb/storage/innobase/handler/ha_innodb.cc:408-422` defines the
  `innodb_instant_alter_column_allowed` modes, and the default server variable
  at `ha_innodb.cc:19204-19216` is `add_drop_reorder`.
- `mariadb/sql/sql_table.cc:8100-8220` upgrades metadata locks for in-place and
  instant ALTER and then runs `lock_tables()` before the alter prepare phase.

## Scope And Non-Goals

- Add focused ownerless SQL coverage where one ownerless process performs
  instant ADD, DROP, and reorder operations while an already-open ownerless peer
  reads and writes the table after each generation change.
- Reuse the existing ownerless dictionary-generation refresh path; do not add a
  new lock, page-version, redo, or dictionary primitive.
- Do not claim full online DDL completion. Index replacement, instant column
  metadata, the follow-on `ownerless-instant-column-variant-refresh` slice, and
  existing DDL stress are evidence for representative classes, not an external
  randomized DDL oracle.
- Do not change public API, directory layout, binary profile, dependencies, or
  storage-engine policy.

## Design

- Extend the existing `ddl-broader` cross-process selector with a new table
  owned by the DDL child.
- The DDL child creates a base InnoDB table, adds a stored column with
  `ALGORITHM=INSTANT, LOCK=NONE`, drops an old stored column with the same
  algorithm, then reorders a stored column with `ALGORITHM=INSTANT, LOCK=NONE`.
- The already-open peer verifies:
  - the instant-added column is visible with the default for pre-existing rows,
  - peer writes can set the instant-added column after the refresh boundary,
  - the dropped column is absent from `information_schema.columns`,
  - the reordered column layout is visible by ordinal position,
  - row data remains writable and readable after each instant metadata change.

## Compatibility Impact

This adds test evidence for MariaDB-compatible `ALTER TABLE ... ALGORITHM=INSTANT`
behavior in ownerless read/write mode. It does not change SQL semantics. The
compatibility matrix should move the broader DDL row forward by naming instant
ADD/DROP/reorder coverage while keeping external randomized DDL stress planned.

## Directory And Lifecycle Impact

Instant ALTER updates native MariaDB/InnoDB dictionary state inside the MyLite
database directory. The existing ownerless dictionary generation and peer
refresh path must make the already-open peer discard stale metadata before it
uses the altered table again. No new durable files are introduced.

## Native Storage Impact

The slice exercises native InnoDB instant metadata, including dropped-column and
column-order metadata. It does not alter native file formats or MyLite's
tablespace replay policy.

## Test Plan

- Build the ownerless SQL test target in `embedded-dev`.
- Run `./build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader`.
- Run focused ownerless labels that cover DDL refresh and primitive state:
  `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`.
- Run the focused `ddl-broader` selector in the unsafe-hook preset.
- Run the ownerless stress preset.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- An already-open ownerless peer sees instant ADD, DROP, and reorder metadata
  after another process publishes the DDL generation stable.
- The peer can continue DML against the instant-altered table after each
  refresh boundary.
- Existing broader DDL coverage still passes.
- Docs and compatibility claims identify instant DDL coverage without claiming
  full randomized DDL validation.

## Risks And Follow-Up

- MariaDB may reject some instant forms depending on table shape or
  `innodb_instant_alter_column_allowed`; this slice uses the default
  `add_drop_reorder` mode and a simple table shape to keep the test deterministic.
- This does not cover every online DDL class, partitioned-table DDL, foreign-key
  rebuild combinations, instant virtual-column reorder variants, or external
  RQG-style DDL stress.
