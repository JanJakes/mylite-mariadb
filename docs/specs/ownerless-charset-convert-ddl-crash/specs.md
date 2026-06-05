# Ownerless Charset Convert DDL Crash

## Problem

Ownerless charset-conversion DDL coverage verifies that an already-open peer
sees `ALTER TABLE ... CONVERT TO CHARACTER SET ... COLLATE ...` metadata
changes and can continue using the converted table. The remaining crash
boundary is a writer killed after MariaDB updates the native table metadata and
storage, but before MyLite publishes ownerless dictionary finish.

This slice adds hook-build recovery evidence for a representative successful
table-wide character-set conversion.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... CONVERT TO CHARACTER SET ... COLLATE ...` through the
  alter-table grammar.
- `mariadb/sql/sql_table.cc` resolves convert/default charset and collation
  attributes during ALTER TABLE preparation, including
  `alter_table_convert_to_charset`.
- `mariadb/sql/sql_show.cc` exposes converted column metadata through
  `information_schema.COLUMNS.CHARACTER_SET_NAME` and `COLLATION_NAME`.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
  dictionary DDL and exposes the unsafe `dictionary-before-finish` hook after
  native SQL execution but before ownerless dictionary finish.

## Design

Add an unsafe-hook selector, `dictionary-charset-convert-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates `app.ownerless_charset_convert_base` with
  `DEFAULT CHARSET=latin1 COLLATE=latin1_swedish_ci`,
- inserts two rows and verifies the original column charset/collation through
  `information_schema.COLUMNS`,
- keeps a live ownerless peer open,
- kills a writer after
  `ALTER TABLE app.ownerless_charset_convert_base CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci`
  completes natively but before ownerless dictionary finish,
- verifies live-peer cleanup remains busy until no-live recovery,
- verifies the recovered column charset/collation through
  `information_schema.COLUMNS`,
- verifies retained rows, inserts one post-recovery row, and
- verifies ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for a representative
  table-wide charset conversion.
- Recovered native `.frm` and `.ibd` files under `datadir/app/`.
- Retained rows and post-recovery DML through the recovered table.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive charset/collation matrices.
- Index prefix-width changes caused by wider character sets.
- Randomized DDL oracle execution.
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
ALTER TABLE compatibility evidence by proving a completed native
charset-conversion update survives writer death at MyLite's dictionary
publication boundary.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB table metadata
and file-per-table storage, ownerless live-peer cleanup blocking, no-live
recovery, forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

Charset conversion is executed by MariaDB's native ALTER TABLE machinery.
MyLite coordinates the ownerless dictionary boundary and verifies durable
reopen behavior for the resulting native table.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-charset-convert-crash`
- Run adjacent `charset-convert-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shard, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovery exposes converted `utf8mb4_general_ci` metadata through
  `information_schema.COLUMNS`.
- Retained rows survive and post-recovery DML succeeds.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers one successful charset conversion, not every collation family or
  prefix-width edge case.
- Broader randomized DDL oracle execution remains planned.
