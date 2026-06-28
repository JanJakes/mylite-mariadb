# Ownerless Column Copy-Lock Live Recovery

## Problem Statement

The focused ADD COLUMN copy-lock slice proved that a single stored-column
mutation can recover live when the ALTER statement ends with
`, ALGORITHM=COPY, LOCK=EXCLUSIVE`. The sibling real column mutation recovery
classifiers for `DROP COLUMN` and `MODIFY COLUMN` still required only optional
semicolons after the column clause, so completed MariaDB copy-lock rebuilds for
those supported spellings stayed on the conservative no-live recovery path.

MyLite should classify exact copy-lock DROP and MODIFY COLUMN statements as
recoverable after native success, keep native file-operation marker retention
while another ownerless peer remains live, and drain the marker after final
no-live checkpoint proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `ALTER TABLE` column changes through
  MariaDB's normal DDL path while preserving explicit ALTER options.
- `mariadb/sql/sql_table.cc` executes `ALGORITHM=COPY` ALTERs through a native
  table-copy rebuild and can produce file-operation redo before SQL returns
  success.
- `packages/libmylite/src/database.cc` already has the exact
  `consume_ownerless_optional_copy_exclusive_alter_tail()` helper used by other
  focused rebuild classifiers. The real DROP and MODIFY COLUMN classifiers did
  not call it.

## Scope And Non-Goals

In scope:

- Accept exact `, ALGORITHM=COPY, LOCK=EXCLUSIVE` tails after:
  - `ALTER TABLE ... DROP [COLUMN] <column> [RESTRICT|CASCADE]`
  - `ALTER TABLE ... MODIFY [COLUMN] <column> <definition>`
- Add hook-build crash coverage for copy-lock DROP and MODIFY COLUMN writers
  killed at `dictionary-before-finish`.
- Verify metadata, retained rows, post-recovery writes, marker retention while a
  live peer remains open, marker drain after no-live recovery,
  ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- `CHANGE COLUMN`, `RENAME COLUMN`, generated-column, placement,
  `AUTO_INCREMENT`, index, constraint, and multi-action column ALTER forms.
- Other explicit option orders or broader randomized ALTER rebuild fuzzing.
- SQL-level table-lock callback coverage.

## Compatibility Impact

The change extends live crash recovery for MariaDB-compatible explicit
copy-lock ALTER spellings. SQL semantics remain MariaDB-owned; MyLite only
broadens the set of completed native DDL boundaries that can be finished while
another ownerless peer is live.

## Directory And Native Storage Impact

No directory layout changes are introduced. The covered statements use MariaDB
native InnoDB copy-rebuild behavior inside the MyLite database directory and the
existing ownerless native file-operation marker in
`concurrency/mylite-concurrency.ckpt`.

## Test And Verification Plan

- Add direct hook selectors:
  - `dictionary-column-drop-copy-lock-crash`
  - `dictionary-column-modify-copy-lock-crash`
- Register standalone CTests for both selectors.
- Run both direct selectors and the adjacent column crash CTest subset.
- Run DDL stress, production build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Copy-lock DROP and MODIFY COLUMN writers killed before ownerless dictionary
  finish recover while another ownerless peer remains live.
- Recovered metadata and table contents match the completed MariaDB native DDL.
- Post-recovery writes exercise the new dropped/modified shape.
- The native file-operation marker remains set until the live peer closes and
  drains after final no-live recovery.
- Ownerless reopen, forced `.shm` rebuild, and native exclusive reopen all see
  the same recovered table state.
