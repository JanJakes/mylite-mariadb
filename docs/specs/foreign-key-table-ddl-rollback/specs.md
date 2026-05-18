# Foreign-Key Table DDL Rollback

## Goal

Extend representative failed table-DDL rollback coverage to MyLite-routed
tables with foreign-key metadata. A failed multi-table `DROP TABLE` or
`RENAME TABLE` must restore not only table records, rows, and indexes, but also
the separate child FK records that drive information-schema, `SHOW CREATE
TABLE`, and row checks.

## Non-Goals

- Do not add general transactional DDL.
- Do not cover views, triggers, routines, partitions, temporary tables, or
  cross-schema rename matrices.
- Do not cover cyclic FK action graphs or exhaustive FK action matrices.
- Do not add crash-safe rollback if the process dies while restoring a failed
  statement checkpoint.
- Do not physically reclaim pages made unreachable by failed DDL attempts.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:1420-1464` iterates multi-table drops.
- `mariadb/sql/sql_table.cc:1687-1714` calls engine drop hooks before later
  errors may be reported for a multi-table drop.
- `mariadb/sql/sql_rename.cc:276-327` validates rename pairs and can report a
  missing source after an earlier pair was processed.
- `mariadb/sql/sql_rename.cc:505-562` processes rename pairs sequentially and
  returns errors after MariaDB's DDL-log revert path runs.
- `packages/libmylite/src/database.cc:is_storage_outer_checkpoint_sql()` wraps
  file-backed `DROP` and `RENAME` statements in a MyLite statement checkpoint.
- `packages/libmylite/src/database.cc:mylite_exec()` rolls back the checkpoint
  when `mysql_query()` returns an error.
- `packages/mylite-storage/src/storage.c:mylite_storage_drop_table()` removes
  child FK records when dropping a child table.
- `packages/mylite-storage/src/storage.c:mylite_storage_rename_table()` rewrites
  child and parent FK identity records during table rename.

## Compatibility Impact

Representative failed table-DDL rollback now includes FK catalog records for
the supported FK subset. This keeps the broader rollback claim bounded:
ordinary direct file-backed failed statements are covered, while durable DDL
inside explicit SQL transactions, savepoint-integrated DDL, and crash during
rollback remain planned.

## Design

No production change is expected if existing checkpoint behavior covers all
catalog record types:

1. Create a parent/child FK pair with rows.
2. Run `DROP TABLE child, missing` so the child drop path can remove table and
   FK records before the statement returns an error.
3. Verify the statement checkpoint restores the child table, parent table, FK
   metadata, and row checks.
4. Run `RENAME TABLE child TO renamed, missing TO other` so rename metadata
   rewrites can be rolled back.
5. Verify the original child name, FK metadata, and row checks remain visible
   before and after close/reopen.

If this exposes partial FK metadata publication, fix the checkpoint or storage
catalog mutation path rather than special-casing the test.

## File Lifecycle

The lifecycle remains inside the primary `.mylite` file plus permitted
transient MyLite recovery-journal files. No persistent `.frm`, `.ibd`, `.MYD`,
`.MYI`, `.MAI`, `.MAD`, Aria log, binlog, relay log, or plugin-owned durable
table file is introduced.

## Embedded Lifecycle And API

No public C API change is required. The behavior is exposed through
`mylite_exec()` errors and close/reopen discovery.

## Build, Size, And Dependencies

No dependency or size-profile change is intended. The test uses the existing
storage-smoke embedded build.

## Test Plan

- Add storage-engine smoke coverage that creates a parent/child FK pair with
  rows.
- Verify failed `DROP TABLE child, missing` preserves parent/child table
  records, child FK metadata, rows, and row checks.
- Verify failed `RENAME TABLE child TO renamed, missing TO other` preserves the
  original child name, rejects the intermediate name, and preserves FK metadata
  plus row checks.
- Verify close/reopen keeps the original parent/child pair and FK metadata
  visible.
- Drop the child and then parent after the rollback checks to prove the
  restored FK metadata still participates in normal cleanup.
- Run the focused storage-smoke test, storage-smoke CTest preset, default CTest
  preset, format check, and `git diff --check`.

## Acceptance Criteria

- Failed child-table multi-drop restores child table and FK metadata.
- Failed child-table multi-rename restores the original child table name and FK
  metadata.
- FK row checks still reject orphan child rows and protected parent key changes
  after each failed DDL statement.
- Close/reopen preserves the restored state.
- Normal child drop and parent drop still work after rollback.

## Implementation Status

Implemented in storage-engine smoke coverage:

- Failed `DROP TABLE child, missing` preserves parent/child table records,
  rows, child FK metadata, information-schema visibility, and row checks.
- Failed `RENAME TABLE child TO renamed, missing TO other` restores the
  original child table name, rejects the intermediate name, and preserves FK
  metadata plus row checks.
- Close/reopen rediscovers the restored parent/child pair and FK metadata.
- Normal child drop and then parent drop still work after the rollback checks.
