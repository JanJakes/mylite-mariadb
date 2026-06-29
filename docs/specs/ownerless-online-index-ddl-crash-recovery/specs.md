# Ownerless Online Index DDL Crash Recovery

## Problem Statement

Ownerless peer-refresh coverage already exercises accepted online secondary
index option forms such as `ALTER TABLE ... ADD INDEX ..., ALGORITHM=INPLACE,
LOCK=NONE`, and hook crash coverage already proves ordinary secondary-index
create/drop recovery. The precise dictionary recovery classifier, however,
only accepts the index clause when it ends immediately. That leaves the online
`ALGORITHM`/`LOCK` tail forms without the same file-operation marker and
live-peer recovery evidence at the `dictionary-before-finish` crash boundary.

This slice narrows that DDL/file-lifecycle gap for the online index form used
by ownerless DDL stress: add/drop secondary index with `ALGORITHM=INPLACE,
LOCK=NONE`.

Non-goals:

- enable unsupported special indexes, partitions, `DISCARD/IMPORT TABLESPACE`,
  or table-directory DDL in ownerless mode,
- claim SQL-level table-lock fault injection is reachable,
- replace randomized external MariaDB/RQG DDL stress.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_alter.cc:65` through `mariadb/sql/sql_alter.cc:98`
  recognize `ALGORITHM=INPLACE`, `ALGORITHM=NOCOPY`, `ALGORITHM=INSTANT`,
  `LOCK=NONE`, `LOCK=SHARED`, `LOCK=EXCLUSIVE`, and `LOCK=DEFAULT`.
- `mariadb/sql/sql_alter.cc:138` through `mariadb/sql/sql_alter.cc:218`
  validate requested algorithm and lock clauses against the storage engine's
  in-place support result.
- `mariadb/sql/sql_table.cc:11589` through `mariadb/sql/sql_table.cc:11713`
  route non-copy ALTER through `fill_alter_inplace_info()`, create the altered
  table definition, request handler support, and apply the requested lock mode.
- `mariadb/mysql-test/main/alter_table.test:1513` through
  `mariadb/mysql-test/main/alter_table.test:1515` exercise accepted
  `ADD INDEX` option combinations including `ALGORITHM=INPLACE, LOCK=NONE`.
- `packages/libmylite/src/database.cc` classifies ownerless dictionary DDL
  before executing MariaDB SQL and marks recoverable/file-operation state before
  publishing the ownerless dictionary generation. A killed writer at
  `dictionary-before-finish` therefore needs the same precise recovery kind for
  the online option spelling as for plain secondary-index DDL.

## Design

Add a MyLite-owned SQL-token helper that consumes an optional trailing
online-index ALTER tail:

- no tail, or only semicolons, remains accepted;
- `, ALGORITHM=<accepted>, LOCK=<accepted>` is accepted;
- `, LOCK=<accepted>, ALGORITHM=<accepted>` is accepted;
- accepted algorithm values are `INPLACE`, `NOCOPY`, and `DEFAULT`;
- accepted lock values are `NONE`, `SHARED`, `EXCLUSIVE`, and `DEFAULT`.

Use that helper only for ownerless ALTER-index recovery classifiers for this
slice. It is intentionally not a general ALTER parser; MariaDB remains the SQL
authority for actual execution.

Add two unsafe-hook selectors:

- `dictionary-online-index-option-crash`: create a table, kill a writer after
  `ALTER TABLE ... ADD INDEX ..., ALGORITHM=INPLACE, LOCK=NONE`, recover while
  a peer is still live, require the native file-operation marker to remain set
  until final no-live recovery, then verify index metadata and forced-index
  reads through ownerless reopen, native reopen, forced `.shm` rebuild, and
  native reopen after rebuild.
- `dictionary-online-index-option-drop-crash`: same pattern for
  `ALTER TABLE ... DROP INDEX ..., ALGORITHM=INPLACE, LOCK=NONE`, verifying the
  index is absent and `FORCE INDEX` fails after recovery.

## Compatibility Impact

No new SQL feature is enabled. This strengthens the evidence for existing
accepted MariaDB online secondary-index DDL option forms in ownerless
read/write mode.

## DDL Metadata Routing Impact

The covered statements still route through MariaDB `ALTER TABLE` and InnoDB
online DDL. MyLite only recognizes the tail spelling so its ownerless
dictionary recovery kind matches the native file-operation class.

## Directory And Lifecycle Impact

No directory layout change. The tests exercise existing ownerless
`mylite-concurrency.wal`, `.shm` rebuild, live-peer recovery, native
file-operation marker retention, final no-live marker drain, and ordinary
native exclusive reopen.

## Native Storage Impact

Native InnoDB files and index metadata remain unchanged. The slice proves that
after a killed online-index DDL writer, the native file-operation marker is not
lost before no-live checkpoint proof makes native files authoritative.

## Public API, Wire Protocol, Build, Size, License, And Dependencies

No public API, wire-protocol, build-profile, binary-size, license, or
dependency changes.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `dictionary-online-index-option-crash`
  - `dictionary-online-index-option-drop-crash`
- Run the focused CTest names for those selectors.
- Run adjacent index metadata crash selectors.
- Run the online DDL option refresh selector.
- Run ownerless DDL stress subset.
- Run production build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The option-tail classifiers return precise create/drop index recovery kinds.
- Both focused selectors reach `dictionary-before-finish` and recover while a
  peer remains live.
- The native file-operation marker remains set while the live peer is held and
  drains after final no-live recovery.
- Final ownerless/native reopen before and after forced `.shm` rebuild observe
  the recovered present/absent index state.
- Compatibility docs and the ownerless concurrency spec record the new bounded
  evidence and keep broader randomized DDL stress planned.

## Risks And Unresolved Questions

- Deterministic crash coverage does not replace randomized DDL oracle stress.
- The helper intentionally accepts only the simple option tail used by MariaDB
  online DDL option clauses; mixed multi-clause ALTER combinations remain
  governed by their existing focused classifiers.
- SQL-level table-lock fault injection remains unproved because explored SQL
  shapes have not reached the ownerless table-wait callback.
