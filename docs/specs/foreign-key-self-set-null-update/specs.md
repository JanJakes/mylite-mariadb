# Foreign-Key Self SET NULL Update

## Goal

Extend MyLite's bounded self-referencing FK action support from
`ON DELETE SET NULL` to `ON UPDATE SET NULL` for simple durable tables. Updating
a parent key should set matching child FK columns in other rows to SQL `NULL`
before publishing the parent-row update.

## Non-Goals

- Non-self `ON UPDATE SET NULL`, because mutating another SQL table from the
  handler needs a separate table-opening, locking, and recursion design.
- Cascades, `SET DEFAULT`, or recursive action chains.
- Same-row parent-key and child-key rewrites where the row being updated also
  references the old parent key; those remain part of the broader FK action
  matrix.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action support.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_class.h:Foreign_key` records `FK_OPTION_SET_NULL` for both
  update and delete actions.
- `mariadb/sql/table.cc` renders `ON UPDATE SET NULL` through stored FK
  metadata for `SHOW CREATE TABLE`.
- `mariadb/storage/innobase/include/dict0mem.h:dict_foreign_t` stores action
  flags and exact child/referenced indexes for engine-owned checks.
- `mariadb/storage/innobase/row/row0ins.cc` executes update and delete actions
  inside the engine row path and prevents recursive cascade overrun.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` currently runs
  child checks, then parent checks, then publishes the updated parent row. The
  self-table action can reuse the existing same-table row mutation machinery
  before the parent check.

Official MariaDB documentation describes `SET NULL` as an FK action and
requires child columns to allow `NULL`:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Compatibility Impact

MyLite can claim a second bounded FK action path: self-referencing
`ON UPDATE SET NULL` over simple nullable child columns, excluding same-row
parent/child rewrites. Non-self actions, cascades, recursive action chains, and
same-row update action edge cases remain planned.

## Design

DDL validation accepts `ON UPDATE SET NULL` under the same table-shape
constraints as self-referencing `ON DELETE SET NULL`: same logical table,
nullable child FK columns, no BLOB/TEXT fields, and no generated columns.

During `ha_mylite::update_row()`, after the new row's child FK checks pass and
before parent FK checks run, MyLite lists parent FK metadata for the updated
table. For a self-referencing update `SET NULL` action, it:

1. encodes the old parent key into the child-side key format;
2. returns early if the old parent key is `NULL` or unchanged;
3. scans live rows from the same MyLite table;
4. skips the row currently being updated;
5. sets matching child FK columns in other rows to SQL `NULL`;
6. recomputes index entries and row payloads;
7. reruns duplicate-key, child-FK, and parent-FK checks for each child update;
8. publishes those child-row updates before the original parent update
   continues.

The existing parent FK check remains active after the action. That is
intentional: it catches unsupported same-row cases and any other remaining
references before the parent row update is published.

## File Lifecycle

No file-format or companion-file change is introduced. Action side effects are
ordinary MyLite row and index updates inside the primary `.mylite` file and are
covered by the existing statement checkpoint.

## Embedded Lifecycle And API

No public API change. Behavior is visible through normal SQL and
`SHOW CREATE TABLE` metadata.

## Build, Size, And Dependencies

No build-profile, binary-size, dependency, or license change.

## Test Plan

- Accept self-referencing `ON UPDATE SET NULL` DDL and preserve
  `SHOW CREATE TABLE` output across close/reopen.
- Verify parent-key updates set multiple child rows to `NULL`, preserve indexed
  reads, and leave old parent keys unusable.
- Verify action side effects roll back when another FK blocks the parent
  update.
- Reject non-self `ON UPDATE SET NULL` and non-nullable child columns before
  catalog publication.
- Run `git diff --check`, rebuild the storage-smoke MariaDB archive, run the
  focused storage-engine smoke binary, and run storage-smoke/default CTest
  presets.

## Acceptance Criteria

- The supported self-referencing update action works for direct SQL and survives
  close/reopen.
- Unsupported action shapes fail before catalog publication.
- Child action updates plus the parent update remain statement-atomic.
- Docs and compatibility matrices distinguish this bounded path from broader
  FK action support.

## Risks And Open Questions

- Same-row update actions still need a design that can reason about the current
  row's old and new child key entries without reading stale index state.
- The implementation scans same-table rows for action targets. That is
  acceptable for the current storage engine stage; targeted child-index scans
  can replace it later if needed.
