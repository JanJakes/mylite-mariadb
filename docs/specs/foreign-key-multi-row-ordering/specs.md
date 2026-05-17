# Foreign-Key Multi-Row Ordering

## Problem

MyLite already performs immediate child and parent foreign-key checks for the
supported `RESTRICT` / `NO ACTION` subset, but the roadmap still marks
multi-row FK ordering as planned. The missing slice is to make batched statement
behavior explicit and covered: earlier successful row operations in the same
statement may affect later FK checks, failed later rows must roll back earlier
row mutations for covered statement-rollback paths, and MyLite must not claim
deferrable or set-wide FK validation.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:mysql_insert()` iterates `values_list` and calls
  `Write_record::write_record()` for each accepted row. `Write_record`
  eventually calls `handler::ha_write_row()` for ordinary inserts.
- `mariadb/sql/handler.cc:handler::ha_write_row()`,
  `handler::ha_update_row()`, and `handler::ha_delete_row()` delegate each row
  to the storage-engine `write_row()`, `update_row()`, or `delete_row()`.
- `mariadb/sql/sql_update.cc` scans matching rows and calls
  `handler::ha_update_row()` for each non-batched update. MyLite does not
  advertise bulk update support, so its FK checks run row by row.
- `mariadb/sql/sql_delete.cc` calls `TABLE::delete_row()`, which delegates to
  `handler::ha_delete_row()` for each deleted row.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_check_foreign_constraint()`
  treats FK checks as immediate per-entry checks and skips them when
  `foreign_key_checks` is off or any participating FK field is SQL `NULL`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`,
  `update_row()`, and `delete_row()` currently call
  `mylite_check_child_foreign_keys()` and
  `mylite_check_parent_foreign_keys()` before publishing each row mutation.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_same_row_self_reference()`
  already permits same-row self references when child and referenced key
  prefixes match in the new row.

## Design

Keep MyLite's supported FK subset immediate and non-deferrable:

1. A multi-row child insert succeeds when each row's parent key is either
   `NULL`, already durable, inserted by an earlier row in the same statement, or
   the same row's self-reference key.
2. A multi-row child insert fails when a row references a parent key that only
   appears in a later row. The statement rollback checkpoint must remove any
   rows inserted earlier in the failed statement.
3. A multi-row self-referential insert uses the same rule: parent-before-child
   and same-row self references are allowed; child-before-parent remains
   rejected.
4. Parent update/delete checks stay immediate per row. This slice does not
   introduce deferrable set-wide validation, cascades, `SET NULL`, or
   transaction-wide FK graph checks.
5. The compatibility matrix should describe multi-row ordering as a covered
   partial behavior, not as full SQL-standard deferred constraint support.

## Compatibility Impact

This matches the immediate FK behavior that applications generally expect from
MySQL/MariaDB `InnoDB` `RESTRICT` / `NO ACTION` constraints: checks happen as
rows are processed, not at statement end as deferrable constraints. It narrows a
roadmap gap by making MyLite's ordering contract explicit for the supported FK
subset.

## Affected Subsystems

- `packages/libmylite/tests/embedded_storage_engine_test.c`: SQL regression
  coverage for ordered multi-row child and self-referential FK statements.
- Documentation and compatibility matrices: move multi-row FK ordering from
  planned to covered partial behavior.
- `mariadb/storage/mylite/ha_mylite.cc`: code changes only if the tests reveal
  a mismatch with the design.

## Single-File And Embedded Lifecycle

No file-format change is intended. The behavior uses existing row/index pages,
FK metadata blobs, and statement rollback checkpoints in the primary `.mylite`
file. No durable MariaDB sidecar or new MyLite companion file is introduced.

## Public API, Build, Size, And Dependencies

No public `libmylite` C API, build profile, dependency, or size-profile change
is intended.

## Test Plan

- Multi-row child insert with an existing parent succeeds.
- Multi-row child insert with a later missing parent fails and rolls back any
  earlier inserted child rows from the same statement.
- Multi-row self-referential insert succeeds for parent-before-child and
  same-row self-reference ordering.
- Multi-row self-referential insert fails for child-before-parent ordering and
  rolls back rows inserted earlier in that statement.
- Repeat representative visibility checks after close/reopen.
- Run focused storage-engine build/test targets, `ctest --preset
  storage-smoke-dev`, `ctest --preset dev`, and `git diff --check`.

## Acceptance Criteria

- MyLite has SQL-level coverage for ordered multi-row FK checks in the
  supported `RESTRICT` / `NO ACTION` subset.
- Failed multi-row FK statements do not leak earlier row mutations.
- Close/reopen discovery preserves the rows accepted by successful statements
  and excludes rejected rows.
- Docs no longer list multi-row FK ordering as fully planned for the current
  supported subset; cascades, `SET NULL`, `SET DEFAULT`, and deferrable
  semantics remain planned/unsupported.

## Implementation Status

Implemented in this slice:

- SQL storage-engine coverage accepts multi-row child inserts that reference an
  existing parent and rejects later missing-parent rows with statement rollback.
- SQL storage-engine coverage accepts parent-before-child and same-row
  self-referential multi-row inserts.
- SQL storage-engine coverage rejects child-before-parent self-referential
  multi-row inserts and verifies earlier rows from the failed statement do not
  persist.
- Reopen checks verify accepted rows remain visible and rejected rows remain
  absent.

## Risks And Open Questions

- Delete/update statements that modify both parent and child rows in one
  statement are still immediate per-row behavior, not set-wide deferred
  validation. Broader statement-order matrices can expand this slice later.
- The test suite should not rely on optimizer-dependent delete/update row order
  unless the SQL includes a deterministic `ORDER BY` and MyLite/MariaDB both
  demonstrably honor it.
