# Ownerless Dictionary Crash CI Attribution

## Problem

Several high-risk ownerless dictionary crash selectors were implemented in the
weighted ownerless SQL shard list but were not registered as standalone CTests.
That made CI failures and timings for generated-column failed-DDL cleanup,
delayed trigger dependency metadata, trigger stored-function bodies, and
`ALTER TABLE ... AUTO_INCREMENT` high-watermark recovery visible only through a
large shard.

The concurrency completion work needs these boundaries to remain independently
selectable while broader DDL/file-lifecycle recovery and performance work
continues.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_trigger.cc:418-720` routes `CREATE TRIGGER` and
  `DROP TRIGGER` through trigger-level MDL, table open/prepare-for-rename, and
  native `.TRG`/`.TRN` metadata writes before the statement reaches MyLite's
  dictionary-finish hook boundary.
- `mariadb/sql/sp_head.cc:1724-1860` and
  `mariadb/sql/sp_head.cc:1896-2180` execute trigger, function, and procedure
  bodies after stored metadata has been resolved, which is why ownerless tests
  need separate coverage for trigger metadata that survives while stored
  routine execution remains policy-rejected.
- `mariadb/sql/sql_table.cc:3230-3415` prepares generated-column and index
  metadata during table create/alter, including virtual fields and generated
  column table definitions.
- `mariadb/sql/sql_table.cc:8640-8745` carries
  `HA_CREATE_USED_AUTO` and auto-increment state through `ALTER TABLE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:17865-17915` treats
  explicit auto-increment value changes as incompatible data so InnoDB handles
  them through the native alter path instead of a metadata-only fast path.

## Design

Register the existing ownerless hook selectors as standalone CTests:

- `dictionary-generated-column-failed-crash`
- `dictionary-trigger-invalid-dependency-crash`
- `dictionary-trigger-stored-function-crash`
- `dictionary-auto-inc-crash`

The existing generated-column foreign-key crash selectors are also grouped with
these standalone dictionary metadata/file-lifecycle tests so the property block
documents a single high-risk generated-column/trigger/AUTO_INCREMENT family.

No product behavior changes in this slice. The weighted shard remains the broad
coverage path; standalone CTests provide isolated timing, targeted reruns, and
clear CI failure attribution.

`dictionary-generated-column-success-crash` is deliberately not promoted by
this slice. A focused re-run exposed a stale selector contract: after the first
successful generated-column create crash, the next live-peer startup can remain
`MYLITE_BUSY` with dead process slots and active dictionary state that has no
recoverable kind. That needs a separate recovery slice before the selector is
safe to run as a standalone CI job.

## Compatibility Impact

This slice does not expand SQL support. It makes existing ownerless recovery and
policy evidence more visible:

- generated-column invalid create/alter/index rejection cleanup;
- generated-column foreign-key add/drop crash recovery through the existing
  grouped standalone selectors;
- trigger metadata that references a missing dependency;
- trigger metadata that references a stored function while ownerless routine
  execution stays rejected;
- `ALTER TABLE ... AUTO_INCREMENT` native high-watermark persistence.

## Directory And Native Storage Impact

No directory layout changes. The promoted selectors continue to verify existing
ownerless/native reopen, forced `.shm` rebuild, native trigger files, InnoDB
table files, and auto-increment state through the existing harness.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run each promoted standalone CTest under `ownerless-test-hooks`.
- Run the matching standalone CTest regex under `ownerless-test-hooks`.
- Run `ctest --preset ownerless-test-hooks -N` to confirm registration.
- Run production guard checks and `git diff --check`.

## Acceptance Criteria

- The four newly promoted selectors are registered as standalone hook CTests.
- The two existing generated-column foreign-key crash selectors are grouped
  with the same ownerless SQL crash labels and bounded timeout.
- The standalone CTests have the same ownerless SQL crash labels and a bounded
  timeout.
- Compatibility docs and the cross-process concurrency spec describe the new
  CI attribution.
- Focused hook verification passes.
