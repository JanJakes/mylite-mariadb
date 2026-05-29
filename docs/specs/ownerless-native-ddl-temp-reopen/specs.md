# Ownerless Native DDL Temp Reopen

## Problem

Ownerless stress coverage exercised concurrent DDL/DML workers and same-name
temporary-table churn, but final reopen checks stayed on ownerless read/write
opens. Native exclusive reopen is now the recovery path that must prove retained
page-version WAL and `.ckpt` seeding work after ownerless peers are gone, so DDL
stress and temporary-table cleanup need the same ordinary read/write reopen
evidence as transaction stress.

## Source Findings

- MariaDB authority remains the repository baseline `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), but this slice changes only
  first-party ownerless replay code, tests, and docs.
- `test_ownerless_ddl_stress()` runs concurrent ownerless DDL creators, DML
  workers, and a reader, then verifies the base table total and cleanup of
  transient DDL stress tables.
- `test_ownerless_temporary_table_stress()` keeps same-named InnoDB temporary
  tables isolated across ownerless peers, releases them, creates a permanent
  table, and verifies it survives ownerless reopen.
- The native exclusive replay path should see the same durable state after all
  ownerless peers have exited, both before and after forced `.shm` rebuild.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` resolves page-version
  records by current tablespace files. A dropped table can leave retained WAL
  records for a space id that no longer has a native file, while strict
  primitive replay should still report an error for unexpected missing
  tablespaces.

## Scope And Non-Goals

This is a no-live recovery and test/docs hardening slice. It does not change
DDL routing, temporary-table behavior, native checkpoint reclamation, public
API, directory layout, dependencies, or binary profile.

## Design

- Keep the strict primitive tablespace replay API unchanged, but add an explicit
  replay flag used by product no-live recovery to ignore page-version records
  for tablespaces that no longer exist. That lets dropped DDL stress tables stop
  blocking recovery of the final database state while preserving strict
  primitive coverage for missing-table corruption.
- Factor DDL stress final-state assertions so they accept open flags.
- Check DDL stress final state through ownerless and ordinary native exclusive
  read/write opens before and after forced `.shm` rebuild.
- Factor temporary stress permanent-table assertions so they accept open flags.
- Check the permanent table through ownerless and ordinary native exclusive
  read/write opens before and after forced `.shm` rebuild.

## Compatibility Impact

No SQL statement behavior changes. Product no-live recovery now ignores
retained page-version records for tablespaces that no longer exist under an
explicit replay flag, and the compatibility matrix has stronger evidence that
ordinary native exclusive reopen preserves ownerless DDL/DML stress output and
post-temporary-table permanent InnoDB state after ownerless peers close.

## Test Plan

- Run focused `ddl-stress` and `temporary-stress` selectors in the ownerless
  stress build.
- Run ownerless primitive coverage for strict missing-tablespace rejection and
  product-mode dropped-tablespace skipping.
- Run the full `ownerless-stress` preset.
- Run formatting and diff checks.

## Acceptance Criteria

- DDL/DML stress final state is visible through ownerless and native exclusive
  reopen before and after forced `.shm` rebuild.
- Temporary-table stress leaves a permanent table visible through ownerless and
  native exclusive reopen before and after forced `.shm` rebuild.
- Product no-live tablespace replay tolerates retained page records for dropped
  tablespaces, while strict primitive replay still rejects missing tablespaces by
  default.
- Existing ownerless stress coverage remains green.
