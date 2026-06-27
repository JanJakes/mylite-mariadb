# Ownerless Temporary Mixed Rename Chain Live Recovery

## Problem Statement

Ownerless crash coverage already proves two-pair mixed temporary/permanent
`RENAME TABLE` lists in both temp-first and permanent-first order. The remaining
mixed rename-list risk is broader statement-order handling: MariaDB can resolve
more than one temporary-table pair in the same rename list while a durable
permanent rename in the middle still needs native file-operation recovery.

MyLite should recover the durable permanent rename while another ownerless peer
remains live, retain the native file-operation marker until no-live checkpoint
proof drains it, and continue treating the killed writer's temporary rename
chain as session-local state that must not become durable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` resolves `RENAME TABLE` pairs in statement order
  and tracks temporary-table pairs before ordinary durable rename execution.
- `mariadb/sql/sql_table.cc` performs non-temporary table renames through the
  native table/file lifecycle path.
- `packages/libmylite/src/database.cc` classifies pure temporary rename lists as
  temporary-table DDL, but mixed lists stay on the durable `RENAME_TABLE`
  recovery lane so permanent file operations remain checkpointed.

## Scope And Non-Goals

In scope:

- Add hook-build crash coverage for a three-pair mixed list:
  temporary shadow to temporary middle, permanent source to permanent target,
  then temporary middle to temporary target.
- Verify live-peer recovery preserves the permanent target and excludes both
  temporary names from durable metadata.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Product SQL behavior changes.
- Fault injection inside MariaDB's internal rename-pair loop.
- Exhaustive mixed rename matrices, `IF EXISTS` variants, and partitioned-table
  rename support.

## Test And Verification Plan

- Add direct hook selector `temporary-mixed-chain-rename-crash`.
- Register standalone CTest
  `libmylite.ownerless-temporary-mixed-chain-rename-crash`.
- Run the new selector with adjacent temporary mixed rename crash selectors.
- Run the relevant ownerless hook CTest subset, format check, and diff check.

## Acceptance Criteria

- A killed mixed rename-chain writer can be recovered while another ownerless
  peer remains live.
- The native file-operation marker remains set while the live peer is open and
  drains after final no-live recovery.
- The durable permanent rename is visible after live recovery, ownerless/native
  reopen, and forced `.shm` rebuild.
- The killed writer's temporary middle and target names do not appear as durable
  tables.
