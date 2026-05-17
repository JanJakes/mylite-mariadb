# Foreign-Key Restrict Enforcement

## Goal

Enforce the first MyLite-owned foreign-key row checks for manually seeded
internal FK metadata while public FK DDL remains rejected. The slice covers
immediate `RESTRICT` / `NO ACTION` behavior for supported MyLite key-prefix
shapes:

- child `INSERT` / `UPDATE` must find a parent key when all child FK columns
  are non-NULL;
- parent `UPDATE` / `DELETE` must reject changes or deletes when a child key
  references the old parent key value.

## Non-Goals

- Accepting `CREATE TABLE` or `ALTER TABLE` foreign-key DDL through
  `libmylite`.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS`. That later review point is covered
  by
  [Foreign-Key Handlerton Advertising](../foreign-key-handlerton-advertising/specs.md).
- Cascading actions, `SET NULL`, `SET DEFAULT`, deferrable checks, dump-import
  `foreign_key_checks=0` semantics, or self-referential statement ordering.
- Cross-file references, partitioned tables, unsupported index classes, or
  virtual generated-column edge cases.
- Full key-shape validation at metadata creation time. Public FK DDL will add
  statement-scoped validation before user-authored constraints are accepted.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/row/row0ins.cc` performs immediate parent and
  child checks for InnoDB FK enforcement and returns `DB_NO_REFERENCED_ROW` or
  `DB_ROW_IS_REFERENCED`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` maps those engine errors to
  `HA_ERR_NO_REFERENCED_ROW` and `HA_ERR_ROW_IS_REFERENCED`.
- `mariadb/sql/handler.cc` turns those handler errors into SQLSTATE `23000`
  errors `ER_NO_REFERENCED_ROW_2` and `ER_ROW_IS_REFERENCED_2`.
- `mariadb/sql/key.h` exposes `key_copy()` and `key_buf_cmp()` over MariaDB's
  handler key encoding. MyLite already uses `key_copy()` for index entries and
  duplicate-key checks.
- `mariadb/storage/mylite/ha_mylite.cc` owns row writes and index-entry
  publication in `write_row()`, `update_row()`, and `delete_row()`, and already
  exposes internal FK metadata through child and parent list hooks.
- `packages/mylite-storage/src/storage.c:mylite_storage_read_index_entries()`
  scans live index-entry pages by table id and numeric index. FK enforcement
  needs a related storage helper that can answer "does any live index entry for
  this table start with this key prefix?" without opening a MariaDB `TABLE`
  object for the other side of the relationship.

Official MariaDB docs describe foreign-key constraints as storage-engine
enforced referential constraints:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Compatibility Impact

Foreign keys remain partial. Public FK SQL still returns the stable MyLite
unsupported-surface diagnostic, but manually seeded internal metadata now has
real DML effects. This moves MyLite closer to the planned narrow FK subset
without yet claiming user-visible FK DDL support.

The compatibility matrix should mention that manually seeded metadata enforces
covered immediate parent/child checks and that cascades, public FK DDL, and
`foreign_key_checks=0` import behavior remain planned.

## Design

Add a first-party storage helper that scans live index-entry pages for a table
and reports whether any stored key starts with a supplied key prefix. The helper
uses the same row-state filtering as `mylite_storage_read_index_entries()`, so
updated and deleted rows are ignored.

In `ha_mylite`:

- for child writes, list FK metadata for the current table, find a supported
  current-table key whose leading columns match the FK child columns, encode
  the child key prefix with `key_copy()`, normalize nullable key-part markers
  to the referenced-side format, skip the check when any child FK column is
  `NULL`, and require a matching parent key prefix in storage;
- for parent updates/deletes, list parent FK metadata for the current table,
  find the current-table referenced key by referenced key name and columns,
  encode the old parent key prefix, normalize nullable key-part markers to the
  child-side format, skip unchanged parent-key updates, and reject when a child
  key prefix exists;
- treat unsupported FK action metadata as fail-closed for parent changes by
  rejecting referenced parent updates/deletes instead of silently omitting a
  cascade or `SET NULL`;
- keep volatile zero-file tables and public FK DDL out of scope.

The first enforcement slice deliberately uses stored key-prefix bytes for the
other table instead of opening a second MariaDB `TABLE` object from inside the
handler. Public FK DDL must later add stricter metadata publication that binds
validated key shapes and can reject ambiguous or unsupported definitions before
they become user-visible constraints.

## File Lifecycle

No new persistent file type is introduced. FK metadata remains in the primary
`.mylite` catalog, and row/index mutations continue through existing statement
checkpoints and recovery journals. Failed FK-enforced DML must leave catalog,
row, index, and autoincrement visibility unchanged through the existing
statement rollback path.

## Embedded Lifecycle And API

No public C API additions are needed. Direct and prepared DML return ordinary
MariaDB integrity errors for covered FK violations. Direct and prepared FK DDL
continue to reject before MariaDB execution with the existing MyLite diagnostic.

## Build, Size, And Dependencies

No new dependency is needed. The fork delta is limited to the MyLite handler,
first-party storage code, tests, and docs. Binary-size impact should be
negligible and does not need a separate measurement unless the build profile
changes unexpectedly.

## Test Plan

- Storage unit tests for the new live key-prefix existence helper, including
  update/delete row-state filtering.
- Storage-smoke tests with manually seeded FK metadata:
  - child insert/update without a parent rejects with SQLSTATE `23000`;
  - child insert/update with a parent succeeds;
  - child rows containing `NULL` in any FK column skip parent lookup;
  - parent update/delete with referencing children rejects;
  - unrelated child updates and unrelated parent updates remain allowed when
    the covered key values do not violate the constraint;
  - checks continue after close/reopen;
  - public FK DDL remains rejected.
- Compatibility harness coverage stays under the existing `foreign-key` group.
- Verification: targeted storage and storage-smoke tests, formatting checks,
  and `git diff --check`.

## Acceptance Criteria

- Covered child DML returns `HA_ERR_NO_REFERENCED_ROW` behavior when the parent
  key is missing.
- Covered parent DML returns `HA_ERR_ROW_IS_REFERENCED` behavior when live
  child rows reference the old parent key.
- Existing FK metadata surfaces, copy-ALTER checks, `DROP TABLE`/`RENAME TABLE`
  behavior, and public FK DDL rejection continue to pass.
- Docs and compatibility status distinguish internal seeded enforcement from
  public FK SQL support.

## Risks And Open Questions

- Raw key-prefix matching relies on validated compatible key shapes. That is
  acceptable for internal seeded metadata but not enough for public FK DDL.
- Opening the other table's MariaDB `TABLE` object inside the handler would
  provide richer validation but risks lock recursion and larger fork deltas.
- Self-referential inserts and cascades need separate statement-ordering and
  action semantics before they can be claimed.
- `foreign_key_checks=0` is still an import-compatibility design problem, not
  a bypass in this slice.
