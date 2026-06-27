# Ownerless Temporary Mixed Rename Order Live Recovery

## Problem Statement

Ownerless crash coverage already proves a mixed temporary/permanent
`RENAME TABLE` list where the temporary pair appears before the durable
permanent pair. The remaining mixed-order gap is the reverse ordering:
MariaDB executes the permanent file rename first, then resolves a tracked
temporary-table rename in the same statement before MyLite publishes the
ownerless dictionary generation stable.

That ordering should stay on the durable table-rename recovery lane, retain the
native file-operation marker while a peer remains live, and still treat the
temporary target as session-local state that disappears with the killed writer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` resolves each `RENAME TABLE` pair in statement
  order and annotates pairs that refer to temporary tables before continuing
  through ordinary rename checks.
- `mariadb/sql/sql_table.cc` performs durable non-temporary rename work through
  the native table/file lifecycle path.
- `packages/libmylite/src/database.cc` intentionally classifies pure temporary
  rename lists as `TEMPORARY_TABLE`, but returns false for mixed lists so the
  existing durable `RENAME_TABLE` recovery kind handles the permanent file
  operation.
- Temporary-table recovery now also forces the native file-operation checkpoint
  marker before the injected prefinish crash boundary so temporary-only DDL
  redo is not cleared before a no-live native checkpoint can prove it.

## Scope And Non-Goals

In scope:

- Add hook-build crash coverage for the permanent-first mixed list:
  `RENAME TABLE permanent_src TO permanent_dst, temp_shadow TO temp_shadow_moved`.
- Reuse the existing durable rename recovery kind and native file-operation
  marker behavior.
- Verify the permanent target survives, the permanent source is absent, the
  temporary target is absent as a durable table, and final ownerless/native
  reopen plus forced `.shm` rebuild observe the same state.

Out of scope:

- Product SQL behavior changes.
- Crash injection inside MariaDB's internal rename-pair loop.
- Exhaustive mixed rename matrices, `IF EXISTS` variants, or partitioned-table
  rename support.

## Test And Verification Plan

- Add direct hook selector `temporary-mixed-rename-reverse-crash`.
- Register it as a standalone CTest under the ownerless hook preset.
- Run the new selector with the existing temporary rename crash group and
  temporary stress.
- Run format and diff checks.

## Acceptance Criteria

- A killed permanent-first mixed temporary/permanent `RENAME TABLE` writer can
  be recovered while another ownerless peer remains live.
- The native file-operation marker remains set while the live peer is open and
  drains after final no-live recovery.
- The durable permanent rename is visible after live recovery, ownerless/native
  reopen, and forced `.shm` rebuild.
- Temporary rename targets from the killed writer do not appear as durable
  permanent tables.
