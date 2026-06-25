# Ownerless Table Comment DDL Crash

## Problem

Ownerless table-comment DDL coverage verifies that an already-open peer sees
`ALTER TABLE ... COMMENT='...'` metadata changes. The remaining crash boundary
is a writer killed after MariaDB updates the native table metadata but before
MyLite publishes ownerless dictionary finish.

This slice adds hook-build recovery evidence for a representative successful
table-comment metadata update.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses table-option `COMMENT` and records
  `HA_CREATE_USED_COMMENT` in `HA_CREATE_INFO`.
- `mariadb/sql/sql_table.cc` carries explicit table comments through
  ALTER TABLE preparation and preserves the old comment when no new comment is
  supplied.
- `mariadb/sql/handler.cc` includes `ALTER_CHANGE_CREATE_OPTION` in the
  generic in-place ALTER operation set.
- `mariadb/sql/sql_show.cc` exposes the table comment through
  `information_schema.TABLES.TABLE_COMMENT`.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
  dictionary DDL and exposes the unsafe `dictionary-before-finish` hook after
  native SQL execution but before ownerless dictionary finish.

## Design

Add an unsafe-hook selector, `dictionary-table-comment-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates `app.ownerless_table_comment_base` with
  `COMMENT='ownerless initial comment'`,
- inserts two rows and verifies the initial comment through
  `information_schema.TABLES`,
- keeps a live ownerless peer open,
- kills a writer after
  `ALTER TABLE app.ownerless_table_comment_base COMMENT='ownerless updated comment'`
  completes natively but before ownerless dictionary finish,
- verifies metadata-only live-peer recovery with the native file-operation
  marker clear through
  `docs/specs/ownerless-metadata-alter-live-recovery/specs.md`,
- verifies the recovered table comment through `information_schema.TABLES`,
- verifies retained rows, inserts one post-recovery row, and
- verifies ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for representative table-comment
  ALTER TABLE metadata.
- Recovered native `.frm` and `.ibd` files under `datadir/app/`.
- Retained rows and post-recovery DML through the recovered table.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive table-option crash matrices.
- Comment encoding, collation, and length edge cases.
- Randomized DDL oracle execution.
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
ALTER TABLE compatibility evidence by proving a completed native table-comment
metadata update survives writer death at MyLite's dictionary publication
boundary.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB table metadata,
ownerless live-peer recovery, forced `.shm` rebuild, and ordinary native
exclusive reopen.

## Native Storage Impact

Table comments are MariaDB-native table metadata. MyLite coordinates the
ownerless dictionary boundary and verifies durable reopen behavior for the
resulting native table.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-comment-crash`
- Run adjacent `table-comment-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shard, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Live-peer recovery is covered by
  `docs/specs/ownerless-metadata-alter-live-recovery/specs.md`.
- Recovery exposes the updated table comment through
  `information_schema.TABLES.TABLE_COMMENT`.
- Retained rows survive and post-recovery DML succeeds.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers one successful table-comment rewrite, not every table option or
  comment encoding edge case.
- Broader randomized DDL oracle execution remains planned.
