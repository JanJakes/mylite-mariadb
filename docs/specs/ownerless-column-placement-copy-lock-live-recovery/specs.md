# Ownerless Column Placement Copy-Lock Live Recovery

## Problem

Ownerless hook coverage already proves single-column `ALTER TABLE ... ADD
COLUMN` recovery and the exact `ALGORITHM=COPY, LOCK=EXCLUSIVE` copy-rebuild
shape. Separately, peer-refresh coverage proves already-open peers notice
instant `FIRST` and `AFTER` stored-column placement. The remaining gap was the
crash/live-peer boundary where MariaDB has completed a copy-style placed
`ADD COLUMN` but MyLite has not finished the ownerless dictionary generation.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` accepts optional column-placement tails on
  column-definition ALTER clauses with `FIRST` or `AFTER <column>`.
- `mariadb/sql/sql_table.cc` routes forced copy/exclusive column ALTERs through
  the native table-copy rebuild path before the SQL layer finalizes dictionary
  state.
- `packages/libmylite/src/database.cc` already recognizes exact ownerless
  ADD COLUMN recovery statements and exact copy/exclusive ALTER tails. The
  parser needed to consume the MariaDB placement tail before that existing
  ALTER-level option tail.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` has the
  deterministic `dictionary-before-finish` hook used by adjacent column-copy
  recovery tests.

## Design

The ADD COLUMN recovery parser now allows a top-level placement tail after the
column definition:

- `FIRST` is accepted as a metadata-only placement target.
- `AFTER <column>` is accepted only when the referenced column already exists
  in the target table metadata.

The existing ADD COLUMN recovery kind and native file-operation marker behavior
are reused. No shared-memory or WAL format changes are introduced.

## Scope And Non-Goals

In scope:

- Exact `ALTER TABLE ... ADD COLUMN ... AFTER <existing-column>,
  ALGORITHM=COPY, LOCK=EXCLUSIVE`.
- Exact `ALTER TABLE ... ADD COLUMN ... FIRST, ALGORITHM=COPY,
  LOCK=EXCLUSIVE`.
- Live-peer recovery at `dictionary-before-finish`.
- Native marker retention while the peer remains live, no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild checks.
- Metadata order, default-backed existing rows, and post-recovery insert
  behavior.

Out of scope:

- Multi-action column ALTER lists.
- Other copy/exclusive option orders.
- Generated, AUTO_INCREMENT, index, constraint, and foreign-key column ALTER
  forms.
- SQL-level table-lock fault injection.

## Compatibility Impact

This is a compatibility expansion for ownerless crash recovery only. Normal
MariaDB SQL behavior for placed ADD COLUMN is unchanged, and unsupported forms
remain outside the recognized ownerless recovery subset.

## Directory And Lifecycle Impact

No new files or directory layout are introduced. The existing native
file-operation checkpoint marker remains set while a live peer prevents final
drain, then clears on a no-live ownerless reopen after recovery completes.

## Native Storage Impact

The slice relies on MariaDB/InnoDB to finish the copy-style rebuild. MyLite
recovers the ownerless dictionary generation around that completed native state
and verifies the rebuilt table survives native reopen and forced shared-memory
rebuild.

## Test Plan

- Add a focused hook selector for placed ADD COLUMN copy-lock recovery.
- Kill an `AFTER value` placed ADD COLUMN writer at
  `dictionary-before-finish`, recover with a live ownerless peer, verify
  ordinal position, defaults, writes, marker retention, and no-live drain.
- Repeat the same boundary for `FIRST`.
- Reopen ownerless/native before and after forced `.shm` rebuild and verify
  metadata order, defaults, row sums, and post-recovery insert/delete behavior.
- Run focused hook CTest coverage plus production-build/format/diff checks.

## Acceptance Criteria

- `AFTER <existing-column>` and `FIRST` placement tails are parsed before the
  exact copy/exclusive ALTER tail.
- Missing or malformed `AFTER` targets are not accepted by recovery parsing.
- Both covered crash shapes recover while another ownerless process remains
  live and keep the native file-operation marker until no-live recovery drains
  it.
- Final ownerless and native exclusive opens observe the same column order and
  data after forced shared-memory rebuild.

## Risks

- The parser is intentionally still exact. Broader option orders and
  comma-separated multi-action ALTER lists remain separate completion slices.
- Placement validation consults pre-existing metadata, so later same-statement
  references to newly added columns remain outside this slice.
