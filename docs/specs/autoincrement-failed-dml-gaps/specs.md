# Autoincrement Failed DML Gaps

## Goal

Preserve generated `AUTO_INCREMENT` gaps when a top-level MyLite-routed DML
statement consumes an autoincrement value and then fails or ignores the row.
This completes the immediate follow-up to transaction/savepoint rollback gaps
without making failed DDL autoincrement metadata changes durable.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB's `AUTO_INCREMENT` documentation says values can be reserved by a
  transaction and lost after rollback, and that `AUTO_INCREMENT` values should
  not be treated as gapless numeric sequences.
- MariaDB's InnoDB autoincrement documentation says persistent autoincrement
  does not mean transactional and names failed `INSERT IGNORE`, `ROLLBACK`,
  and `ROLLBACK TO SAVEPOINT` as gap-producing cases:
  <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/auto_increment-handling-in-innodb>.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` fills the row's
  autoincrement field before the handler checks duplicate keys through
  `write_row()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:ha_innobase::get_auto_increment()`
  can update InnoDB's persistent table autoincrement state when reserving an
  interval, before the row is finally committed.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` currently
  advances durable MyLite autoincrement state after duplicate-key and child-FK
  checks. A failed duplicate or FK insert therefore leaves no durable
  autoincrement page for statement rollback to preserve.
- `packages/mylite-storage/src/storage.c:mylite_storage_rollback_statement()`
  can now preserve appended autoincrement pages for transaction and nested
  savepoint rollback. A DML-only marker can safely extend that behavior to
  top-level statement checkpoints.

## Scope

- Add a storage checkpoint marker that says a top-level statement rollback
  should preserve advancing autoincrement pages.
- Mark durable MyLite `write_row()` statements after successful autoincrement
  state publication and before duplicate/FK checks can fail.
- Move durable insert autoincrement publication before duplicate/FK checks so
  failed or ignored inserts have state to preserve.
- Cover generated duplicate-key insert failure, generated `INSERT IGNORE`
  duplicate rows, explicit high-value `INSERT IGNORE`, close/reopen
  persistence, and the storage marker behavior.

## Non-Goals

- Failed `UPDATE` autoincrement semantics.
- Failed DDL autoincrement metadata changes.
- MEMORY/HEAP runtime-volatile failed-DML gaps.
- Handler-level MariaDB savepoint hooks, isolation, WAL, or lock-mode changes.
- Exhaustive `INSERT ... ON DUPLICATE KEY UPDATE` and multi-row mixed-success
  matrices.

## Compatibility Impact

Top-level failed and ignored insert behavior moves closer to MariaDB/InnoDB for
durable MyLite-routed tables. A row that consumed a generated value but did not
become visible no longer lets the next insert reuse that value.

## Design

Add `mylite_storage_preserve_auto_increment_on_rollback(filename)` to mark the
active storage checkpoint for the current primary file. The mark is intentionally
orthogonal to transaction ownership: transaction and nested savepoint
checkpoints always preserve advancing autoincrement pages, while top-level
statement checkpoints preserve them only when DML explicitly marks the
checkpoint.

In `ha_mylite::write_row()` for durable rows:

1. let MariaDB assign the row autoincrement value with
   `update_auto_increment()`;
2. publish any advancing table-local autoincrement value from the row;
3. mark the active checkpoint for autoincrement preservation;
4. run duplicate-key and FK checks;
5. append the row and index entries when checks pass.

The existing rollback preservation logic still republishes only values greater
than the checkpoint-visible next value and only for table IDs that existed in
the checkpoint catalog. That keeps failed DDL rollback bounded: CTAS/ALTER temp
table IDs are not in the checkpoint catalog, and DDL paths that only set table
options do not mark the checkpoint through `write_row()`.

## File Lifecycle

No file-format change is required. Failed-DML gaps are ordinary autoincrement
state pages in the primary `.mylite` file. Top-level failed-statement rollback
restores rows, row-state, index entries, and catalog pages as before, then
republishes marked advancing autoincrement values.

## Embedded Lifecycle And API

No `libmylite` API change is required. The new storage marker is a first-party
storage API used by the MyLite handler.

## Storage-Engine Routing

The behavior applies to durable MyLite-routed tables, including requested
`ENGINE=InnoDB` tables that physically use MyLite storage. Volatile
`MEMORY`/`HEAP` tables remain process-local and out of scope.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced. The
change is storage/handler code plus tests and documentation.

## Test Plan

- Add storage tests for the DML-only preserve marker on a top-level statement
  checkpoint.
- Add SQL storage-engine tests for:
  - generated duplicate-key insert failure;
  - generated duplicate-key `INSERT IGNORE`;
  - explicit high-value `INSERT IGNORE`; and
  - close/reopen next-value persistence.
- Keep failed table-DDL rollback tests green to prove DDL does not inherit the
  DML marker accidentally.
- Run focused storage and storage-engine tests, transaction and
  statement-rollback harness groups, shell syntax checks, and the dev,
  embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- A generated value consumed by a failed duplicate insert is not reused.
- A generated value consumed by `INSERT IGNORE` is not reused.
- An explicit high autoincrement value in an ignored insert advances the next
  value without making the ignored row visible.
- Failed DDL rollback remains covered and green.
- Docs distinguish failed-DML gaps from unsupported failed `UPDATE` and broader
  mixed-statement matrices.

## Risks And Open Questions

- Failed multi-row inserts with a mix of successful, ignored, and failed rows
  need a dedicated matrix before broader claims are made.
- Failed `UPDATE` behavior may differ by storage engine and remains a separate
  compatibility decision.
