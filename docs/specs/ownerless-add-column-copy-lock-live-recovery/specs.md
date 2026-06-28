# Ownerless ADD COLUMN Copy-Lock Live Recovery

## Problem Statement

Ownerless live-recovery coverage already proves a plain stored-column
`ALTER TABLE ... ADD COLUMN` writer killed after native MariaDB success and
before ownerless dictionary finish. A supported explicit rebuild spelling,
`ALTER TABLE ... ADD COLUMN ..., ALGORITHM=COPY, LOCK=EXCLUSIVE`, used the same
native table-copy file lifecycle but was not classified by the focused
single-clause ADD COLUMN recovery parser because the parser stopped at the
comma before the explicit ALTER options.

MyLite should classify that exact copy-lock ADD COLUMN statement as recoverable,
retain the native file-operation marker while another ownerless peer remains
live, and drain the marker only after no-live checkpoint proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `ALTER TABLE` through the normal DDL
  path while preserving explicit `ALGORITHM` and `LOCK` options.
- `mariadb/sql/sql_table.cc` handles `ALGORITHM=COPY` ALTERs through the
  table-copy rebuild path, which can create native file-operation redo evidence
  before SQL statement success returns.
- `packages/libmylite/src/database.cc` already has
  `consume_ownerless_optional_copy_exclusive_alter_tail()` for focused rebuild
  classifiers, but `ownerless_alter_table_add_column_recovery_statement()`
  rejected the comma before that tail.

## Scope And Non-Goals

In scope:

- Accept the exact `, ALGORITHM=COPY, LOCK=EXCLUSIVE` tail after a single
  stored-column ADD COLUMN definition.
- Add hook-build crash coverage for the copy-lock ADD COLUMN statement killed
  at `dictionary-before-finish`.
- Verify recovered metadata/defaults, retained rows, post-recovery writes,
  native file-operation marker retention while a peer remains live, final
  no-live marker drain, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Generated-column, `AUTO_INCREMENT`, placement, multi-action, index, and
  constraint ADD COLUMN forms.
- Other explicit ALTER option orders or broader randomized ALTER rebuild
  fuzzing.
- SQL-level table-lock fault injection.

## Compatibility Impact

This extends ownerless crash recovery for a MariaDB-compatible explicit
copy-lock ALTER spelling. SQL success semantics are still delegated to MariaDB;
MyLite only broadens which completed native DDL boundary can be recovered while
another ownerless peer is live.

## Directory And Native Storage Impact

No directory layout changes are introduced. The statement uses MariaDB native
InnoDB table-copy rebuild behavior inside the MyLite database directory and the
existing ownerless native file-operation marker in
`concurrency/mylite-concurrency.ckpt`.

## Test And Verification Plan

- Add direct hook selector `dictionary-column-add-copy-lock-crash`.
- Register standalone CTest
  `libmylite.ownerless-dictionary-column-add-copy-lock-crash`.
- Run the new selector and the adjacent column crash CTest subset.
- Run production build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The copy-lock ADD COLUMN writer killed before ownerless dictionary finish is
  recovered while another ownerless peer remains live.
- The recovered column has the expected default-backed values and accepts
  post-recovery default inserts.
- The native file-operation marker remains set until the live peer closes and
  drains after final no-live recovery.
- Ownerless reopen, forced `.shm` rebuild, and native exclusive reopen all see
  the same recovered table state.
