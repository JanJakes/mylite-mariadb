# Foreign-Key Self SET NULL Delete

## Problem

MyLite's public foreign-key subset currently accepts and enforces immediate
`RESTRICT` / `NO ACTION` constraints only. That keeps parent update/delete
checks safe, but leaves all action clauses planned. The first useful action
slice is a bounded self-referencing `ON DELETE SET NULL` path, because it uses
the already-open `TABLE` object and proves engine-owned child-row mutation
without opening unrelated tables from inside the handler.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_class.h:Foreign_key` records parsed update/delete actions
  as `enum_fk_option`, including `FK_OPTION_SET_NULL`.
- `mariadb/sql/table.cc` renders stored FK metadata action names for
  `SHOW CREATE TABLE`, including `SET NULL`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` maps accepted
  `FK_OPTION_SET_NULL` definitions into InnoDB dictionary action flags, while
  `FK_OPTION_SET_DEFAULT` is still marked as a TODO in the selected MariaDB
  base.
- `mariadb/storage/innobase/row/row0ins.cc` executes FK actions inside the
  engine row path and enforces cascade-depth limits.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_row()` already owns
  parent-row enforcement for MyLite-routed tables. The current handler can
  safely mutate rows in the same table because the current `TABLE` object has
  the fields, record buffers, key definitions, and row serialization helpers
  needed to recompute child rows and index entries.

Official MariaDB documentation describes `SET NULL` as a storage-engine FK
action and requires the child columns to permit `NULL`:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Implement only this public SQL subset:

- durable MyLite-routed base table;
- self-referencing foreign key;
- `ON DELETE SET NULL`;
- `ON UPDATE` omitted, `RESTRICT`, or `NO ACTION`;
- nullable child FK columns;
- simple row shapes without BLOB/TEXT or generated columns.

## Non-Goals

- Non-self `ON DELETE SET NULL`, because that requires opening and mutating a
  second table from inside the handler.
- `ON UPDATE SET NULL`, `CASCADE`, or recursive action execution.
- `SET DEFAULT`; MariaDB's selected InnoDB base still marks it as unimplemented
  in the engine.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action support.
- Deferrable graph validation or full InnoDB action compatibility.

## Design

DDL validation accepts `ON DELETE SET NULL` only when the FK references the
same logical table, all child FK fields are actually nullable, and the table
shape is simple enough for current MyLite row serialization. Unsupported
action forms keep failing before catalog publication.

On parent delete, MyLite lists parent FK metadata before the parent row is
removed. For a self-referencing `SET NULL` action, it:

1. encodes the old parent key prefix into the child-side key format;
2. scans live rows from the same MyLite table;
3. skips the parent row being deleted;
4. finds child rows whose FK key prefix matches the old parent key;
5. sets the child FK fields to SQL `NULL` in `table->record[0]`;
6. recomputes supported index entries and row payload;
7. reruns duplicate-key, child-FK, and parent-FK checks for the mutated child
   row;
8. publishes the child-row update through
   `mylite_storage_update_row_with_index_entries()`;
9. lets the original parent delete continue.

The existing parent check then ignores that `SET NULL` metadata for the parent
delete because the action has already handled the relevant child rows. Other
parent FKs on the same parent row still enforce their normal immediate checks.

## Compatibility Impact

Foreign keys remain partial. MyLite can now claim a first FK action path:
self-referencing `ON DELETE SET NULL` over the bounded table shape above.
Non-self actions, cascades, update actions, `SET DEFAULT`, generated/BLOB table
actions, and recursive graph cases remain planned or unsupported.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. Action side effects are
ordinary MyLite row and index-entry updates inside the primary `.mylite` file.
Failed action execution or a later parent-delete failure must roll back through
the existing statement checkpoint.

## Tests

- Accept self-referencing `ON DELETE SET NULL` DDL and expose the action through
  `SHOW CREATE TABLE` after close/reopen.
- Reject non-self `ON DELETE SET NULL`, child `NOT NULL` set-null columns, and
  `ON UPDATE SET NULL`.
- Verify deleting a parent row sets multiple child rows to `NULL`, removes the
  parent row, preserves index reads, and survives close/reopen.
- Verify missing-parent child inserts still fail after the action.
- Run storage-smoke and default CTest presets plus `git diff --check`.

## Acceptance Criteria

- The supported self-referencing action works for direct SQL.
- Unsupported action shapes fail before MyLite catalog publication.
- Child-row updates and the parent delete are statement-atomic.
- Docs and compatibility matrices distinguish this first action from broader
  FK action support.

## Risks

- The implementation intentionally avoids opening a second `TABLE` object from
  inside the handler. That keeps the first action small, but non-self actions
  need a separate design for table opening, locking, and recursive checks.
- Current action mutation is bounded to simple table shapes. Generated columns
  and BLOB/TEXT rows need explicit evidence before they are allowed.
