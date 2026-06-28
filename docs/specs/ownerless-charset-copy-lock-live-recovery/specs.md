# Ownerless Charset Copy-Lock Live Recovery

## Problem Statement

Ownerless live DDL recovery already covers `ALTER TABLE ... CONVERT TO
CHARACTER SET ... COLLATE ...` after MariaDB completes the native charset
conversion but before ownerless dictionary finish. The classifier only accepted
semicolon-only tails, so the explicit MariaDB rebuild spelling
`, ALGORITHM=COPY, LOCK=EXCLUSIVE` stayed on the conservative no-live recovery
path even though it uses the same native table-copy boundary class.

MyLite should classify the exact copy-lock charset conversion as live
recoverable, preserve native file-operation marker retention while another
ownerless peer remains live, and drain the marker only after final no-live
checkpoint proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `ALTER TABLE` statements through
  MariaDB's ordinary DDL execution path while preserving explicit ALTER
  options.
- `mariadb/sql/sql_table.cc` accepts table-option ALTER clauses with explicit
  algorithm and lock options, and `ALGORITHM=COPY` can rebuild the native table
  before SQL returns success.
- `packages/libmylite/src/database.cc` already uses
  `consume_ownerless_optional_copy_exclusive_alter_tail()` for focused force,
  engine, row-format, compressed row-format, and column rebuild classifiers.
  The charset conversion classifier still consumed only semicolons.

## Scope And Non-Goals

In scope:

- Accept exact `, ALGORITHM=COPY, LOCK=EXCLUSIVE` tails after:
  - `ALTER TABLE ... CONVERT TO CHARACTER SET <charset>`
  - `ALTER TABLE ... CONVERT TO CHARACTER SET <charset> COLLATE <collation>`
- Add hook-build crash coverage for a charset conversion writer killed at
  `dictionary-before-finish`.
- Verify charset/collation metadata, retained rows, post-recovery writes,
  marker retention while a live peer remains open, marker drain after no-live
  recovery, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Other explicit option orders, multi-action ALTER lists, and randomized
  charset/collation rebuild fuzzing.
- Non-InnoDB charset rebuild classes.
- SQL-level table-lock callback coverage.

## Compatibility Impact

The SQL behavior remains MariaDB-owned. This slice broadens MyLite's live crash
recovery classification for a completed native charset conversion boundary that
MariaDB has already accepted and executed.

## Directory And Native Storage Impact

No directory layout changes are introduced. The covered statement uses MariaDB
native InnoDB table-copy behavior inside the MyLite database directory and the
existing ownerless native file-operation marker in
`concurrency/mylite-concurrency.ckpt`.

## Test And Verification Plan

- Add a direct hook selector:
  - `dictionary-charset-convert-copy-lock-crash`
- Register a standalone CTest for the selector.
- Run the direct selector and adjacent rebuild crash CTests.
- Run DDL stress, production build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The copy-lock charset conversion writer killed before ownerless dictionary
  finish recovers while another ownerless peer remains live.
- Recovered charset/collation metadata and table contents match the completed
  MariaDB native DDL.
- Post-recovery writes use the converted table.
- The native file-operation marker remains set until the live peer closes and
  drains after final no-live recovery.
- Ownerless reopen, forced `.shm` rebuild, and native exclusive reopen all see
  the same recovered table state.
