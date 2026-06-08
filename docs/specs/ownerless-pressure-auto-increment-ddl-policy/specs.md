# Ownerless Pressure AUTO_INCREMENT DDL Policy

## Problem

The ownerless active-reader pressure policy throttles write statements when a
live snapshot pin retains page-version WAL at the configured soft limit. Existing
coverage blocks representative DML, table/index/schema/view/trigger DDL, column
and constraint ALTER variants, generated-column/index DDL, storage rebuild DDL,
and replacement/copy table spellings.

A remaining bounded DDL class is `ALTER TABLE ... AUTO_INCREMENT = N`. It is a
create-option-only ALTER that mutates native InnoDB AUTO_INCREMENT high-watermark
state and is already treated by MyLite as native file-operation checkpoint
evidence. The pressure selector should prove this DDL is also stopped before it
can advance the high watermark while retained WAL is pinned by an active reader.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE` with
  `CF_CHANGES_DATA`, `CF_WRITE_LOGS_COMMAND`, `CF_AUTO_COMMIT_TRANS`, and
  `CF_ADMIN_COMMAND`, so MariaDB treats ALTER TABLE as a native data-changing
  statement class.
- `mariadb/sql/sql_table.cc` `mysql_prepare_alter_table()` carries
  `create_info->auto_increment_value` for ALTER TABLE create options and copies
  the current handler AUTO_INCREMENT value when no explicit value is supplied.
- `mariadb/storage/innobase/handler/handler0alter.cc` `commit_set_autoinc()`
  persists an explicit user-supplied AUTO_INCREMENT value to the clustered index
  metadata with `btr_write_autoinc()`, or derives a safe value from the current
  maximum row when the supplied value is lower than existing rows.
- `packages/libmylite/src/database.cc` classifies leading `ALTER` statements as
  write-like through `sql_statement_requires_write()`, enforces
  `ownerless_page_log_limit_bytes` before statement execution, and separately
  marks any `ALTER TABLE` containing `AUTO_INCREMENT` as needing native file-op
  checkpoint evidence.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with a real
  `ALTER TABLE app.ownerless_pressure_auto_inc_ddl AUTO_INCREMENT = 100`
  statement.
- Verify it returns `MYLITE_BUSY` with the pressure-limit diagnostic while a live
  reader pin retains WAL at the configured limit.
- Verify the blocked attempt leaves the existing rows and native high-watermark
  behavior unchanged.
- Verify the same AUTO_INCREMENT ALTER succeeds after the reader releases and
  the final high watermark survives ownerless/native reopen before and after
  forced `.shm` rebuild.

Out of scope:

- Changing production pressure classification or checkpoint scheduling unless the
  test proves a leak.
- SQL-level table-lock fault injection.
- Broader native redo/checkpoint reconciliation.
- Randomized or external MariaDB/RQG stress.

## Design

Reuse the retained-WAL pressure setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create `app.ownerless_pressure_auto_inc_ddl` with
   `id INT NOT NULL AUTO_INCREMENT PRIMARY KEY` and insert two implicit rows.
2. Hold a repeatable-read snapshot in a peer process.
3. Commit one ownerless update so page-version WAL remains retained by the
   reader pin.
4. Reopen the writer with `ownerless_page_log_limit_bytes` set to the retained
   WAL size.
5. Assert `ALTER TABLE ... AUTO_INCREMENT = 100` returns `MYLITE_BUSY`.
6. While the reader is still pinned, verify the table still has rows `1` and `2`
   and no extra value was inserted or high-watermark mutation was observed.
7. Release the reader and let checkpointing reclaim retained WAL.
8. Insert one implicit row before the successful ALTER and verify it receives
   `id = 3`, proving the blocked ALTER did not persist the `100` high watermark.
9. Execute `ALTER TABLE ... AUTO_INCREMENT = 100`, insert one more implicit row,
   and verify it receives `id = 100`.
10. Reopen ownerless and native handles before and after forced shared-memory
    rebuild and verify count, ID sum, max ID, and value sum.

## Compatibility Impact

No public SQL behavior changes. The slice adds evidence that MyLite's existing
ownerless pressure throttle fails closed before a MariaDB-compatible
AUTO_INCREMENT DDL high-watermark mutation reaches InnoDB while retained WAL is
over the configured limit.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing InnoDB native
AUTO_INCREMENT metadata persistence, ownerless page-version WAL retention,
checkpointing, native file-operation checkpoint evidence, and forced shared
memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked `ALTER TABLE ... AUTO_INCREMENT` must leave
native InnoDB AUTO_INCREMENT metadata untouched; after pressure clears, the same
native MariaDB path must persist and recover the new high watermark.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused `active-reader-pressure-write-policy` selector with
  `php-embedded-prod`.
- Run the ownerless primitives CTest smoke under `php-embedded-prod`.
- Run `cmake --build --preset format`.
- Rebuild the focused test after formatting if needed.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The AUTO_INCREMENT ALTER returns `MYLITE_BUSY` while the pressure limit is
  reached and the reader pin is active.
- The blocked ALTER does not mutate existing rows or the next implicit ID.
- After pressure clears, an implicit insert gets `3`, the successful ALTER
  advances the next implicit ID to `100`, and final aggregate state survives
  ownerless/native reopen before and after forced `.shm` rebuild.
- No production behavior change is required unless the test exposes an existing
  policy leak.

## Verification Results

Local verification on 2026-06-08 used the production `php-embedded-prod` build
tree.

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure-write-policy` passed before formatting and again after
  formatting/rebuild.
- `ctest --preset php-embedded-prod -R 'libmylite\.ownerless-primitives$'
  --output-on-failure` passed.
- `cmake --build --preset format` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.
- No `/tmp/mylite-ownerless-*` or `/tmp/mylite-perf-probe.*` directories were
  left after focused verification completed.

## Risks And Follow-Up

- This is deterministic SQL coverage, not randomized stress.
- The broader durable DDL file-lifecycle and native redo/checkpoint
  reconciliation work remains open.
