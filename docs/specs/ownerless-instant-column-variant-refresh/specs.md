# Ownerless Instant Column Variant Refresh

## Problem

Ownerless DDL coverage already proves explicit InnoDB instant ADD, DROP, and
reorder metadata for a simple stored-column table. The ownerless DDL matrix
previously still called out instant-column variants as planned. MariaDB 11.8
allows more instant metadata-only column changes than the existing shape,
including stored columns inserted at non-tail positions, column renames, and
virtual generated-column add/drop. Already-open ownerless peers must refresh
table metadata across those dictionary-generation boundaries before continuing
reads or writes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc:147-156` defines
  `INNOBASE_ALTER_INSTANT`, including virtual-column order, column rename,
  virtual-column add, rename-index, and virtual-column drop operations.
- `handler0alter.cc:1695-1728` checks stored ADD/DROP/reorder eligibility in
  `instant_alter_column_possible()` while rejecting shapes with full-text or
  indexed virtual-column limitations.
- `handler0alter.cc:2306-2335` applies
  `innodb_instant_alter_column_allowed`; the default MariaDB mode
  `add_drop_reorder` permits stored ADD anywhere, DROP, and reorder.
- `handler0alter.cc:5986-6025` updates native InnoDB instant metadata through
  `innobase_instant_try()`, including SYS_COLUMNS and hidden metadata records.
- `handler0alter.cc:10916-10925` commits instant changes before the separate
  virtual-column add/drop fallback.
- `mariadb/storage/innobase/handler/ha_innodb.cc:411-416` names the instant
  ALTER modes, and `ha_innodb.cc:19208-19212` defaults the variable to
  `add_drop_reorder`.
- `mariadb/sql/sql_table.cc:8113-8220` upgrades MDL for in-place/instant ALTER
  and marks `HA_ALTER_INPLACE_INSTANT` with `LOCK=NONE` as online.

## Scope And Non-Goals

In scope:

- Add focused ownerless SQL coverage for one process applying explicit
  `ALGORITHM=INSTANT, LOCK=NONE` changes while another already-open ownerless
  peer keeps using the table.
- Cover stored-column FIRST placement, stored-column AFTER placement, instant
  column rename, virtual generated-column add, and virtual generated-column
  drop.
- Verify metadata through `information_schema.columns`, peer DML after every
  boundary, and final ownerless/native reopen before and after forced `.shm`
  rebuild.

Out of scope:

- Changing ownerless dictionary-generation, page-version, redo, or native
  checkpoint logic.
- Claiming every instant DDL variant, partitioned-table instant behavior,
  indexed virtual-column instant behavior, virtual-column reorder variants, or
  external MariaDB/RQG randomized DDL coverage.
- SQL-level table-lock fault injection; prior investigation found explored SQL
  shapes are intercepted before the ownerless table-wait callback.

## Design

Add an `instant-column-variants` selector to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The child process owns the DDL sequence:

1. Create a simple InnoDB table and insert one base row.
2. Add a stored `first_note` column at `FIRST` with
   `ALGORITHM=INSTANT, LOCK=NONE`.
3. Add a stored `side_value` column after `base_value` with the same algorithm.
4. Rename stored column `marker` to `renamed_marker` with
   `ALGORITHM=INSTANT, LOCK=NONE`.
5. Add a virtual generated `value_double` column with
   `ALGORITHM=INSTANT, LOCK=NONE`.
6. Drop the virtual generated column with `ALGORITHM=INSTANT, LOCK=NONE`.

The parent keeps its ownerless handle open for the whole sequence. After each
stable generation it queries column ordinal/absence metadata, reads or writes
rows through the refreshed definition, and verifies deterministic aggregates.
Final assertions reopen the database in ownerless read/write and ordinary
exclusive modes before and after deleting the volatile `.shm` file.

## Compatibility Impact

No SQL semantics or public API changes. The slice strengthens ownerless
`ALTER TABLE` compatibility evidence for MariaDB instant column metadata while
keeping broader DDL and randomized oracle coverage partial.

## Directory And Lifecycle Impact

No directory layout changes. The slice exercises native MariaDB dictionary and
InnoDB tablespace metadata inside the MyLite database directory plus the
existing ownerless dictionary generation in `concurrency/mylite-concurrency.shm`.
The final forced `.shm` rebuild checks that durable table metadata and rows do
not depend on volatile coordination state.

## Native Storage Impact

Native InnoDB executes the instant ALTER operations. MyLite does not emulate the
column metadata; it must publish and observe dictionary generations so peers
flush stale table and InnoDB dictionary cache entries before using the table.

## Binary Size And Dependencies

No binary-profile, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `instant-column-variants` in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL CTest.
- Run the hook ownerless cross-process SQL CTest, retrying only known
  intermittent InnoDB log-header checksum aborts after isolated evidence.
- Run the ownerless stress preset if focused and full ownerless checks pass.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes stored FIRST/AFTER instant column
  placement by ordinal position and can write rows using those columns.
- The peer observes instant column rename metadata and cannot use the old name.
- The peer observes virtual generated-column add/drop boundaries, computes
  generated values correctly while the column exists, and continues DML after
  it is dropped.
- Final rows and metadata survive ownerless/native reopen before and after
  forced `.shm` rebuild.
- Documentation names the new bounded variant coverage without claiming full
  randomized DDL or SQL-level table-lock fault coverage.

## Risks And Follow-Up

- MariaDB can reject some instant variants for table shapes with full-text
  indexes or indexed virtual-column constraints. This slice uses a simple
  InnoDB table without those blockers.
- During implementation, MariaDB rejected the attempted virtual generated-column
  reorder with errno 1846 under explicit `ALGORITHM=INSTANT`; this slice records
  accepted virtual add/drop coverage instead of claiming that variant.
- Broader DDL/file-lifecycle recovery metadata, background active-reader
  checkpoint scheduling, SQL-level table-lock fault injection, and external
  MariaDB/RQG stress remain separate ownerless concurrency work.
