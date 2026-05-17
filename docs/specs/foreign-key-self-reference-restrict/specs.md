# Foreign-Key Self-Reference Restrict

## Goal

Cover the bounded self-referential foreign-key subset that MyLite can support
with the current immediate `RESTRICT` / `NO ACTION` infrastructure.

The supported behavior is deliberately narrow: a durable MyLite-routed table
may reference its own exact unique parent key when child rows point at parent
rows that already exist. Immediate child and parent checks should work across
insert, update, delete, truncate, and close/reopen. Same-row self-inserts,
multi-row statement ordering, cascades, `SET NULL`, and `SET DEFAULT` remain
separate work.

## Non-Goals

- Same-row self-referencing inserts such as `(id, parent_id) = (1, 1)`.
- Multi-row statement ordering where a later row in the same statement creates
  a parent for an earlier child.
- Cascades, `SET NULL`, `SET DEFAULT`, or recursive action execution.
- Deferrable constraints.
- Partitioned, volatile, temporary, or row-discarding self-referential FKs.
- Trigger semantics or stored-program interactions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` treats child and
  parent FK metadata separately, and allows a self-referencing FK to be removed
  from parent checks when that same FK is dropped by the ALTER statement.
- `mariadb/sql/sql_truncate.cc:fk_truncate_illegal_if_parent()` rejects
  truncating parent tables referenced by non-self child tables, but permits
  self-referencing FKs by checking that child and parent schema/table names are
  the same.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_validate_foreign_key_shape()`
  has a self-reference path that validates child and parent keys from the same
  `TABLE`/`TABLE_SHARE` without opening a second table definition.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_child_foreign_key()` checks
  parent existence before appending the new row. That makes same-row
  self-reference inserts unsupported until MyLite has a statement-ordering or
  deferred-check design.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_parent_foreign_key()` checks
  child existence by prefix when parent keys are updated or deleted, which
  should protect existing self-referencing parent rows.

## Compatibility Impact

This makes MyLite's documented FK subset more precise:

- covered: self-referential `RESTRICT` / `NO ACTION` constraints where the
  referenced parent row already exists, including close/reopen and
  self-referencing truncate;
- unsupported/planned: same-row self-inserts, multi-row ordering, recursive
  cascades, `SET NULL`, `SET DEFAULT`, and deferrable checks.

## Design

No handler code change is expected. The existing self-reference validation path
and row checks should be exercised through public FK DDL:

1. Create a self-referential routed table with a nullable child column and an
   explicit child key.
2. Insert a root row with `NULL` parent.
3. Insert a child row referencing the existing root.
4. Reject an insert that references a missing row.
5. Reject same-row self-reference until a later ordering slice defines it.
6. Reject updating or deleting a referenced parent row.
7. After deleting the child row, allow the parent update/delete path.
8. Verify `TRUNCATE TABLE` succeeds for the self-referencing table and resets
   rows/autoincrement state because MariaDB permits self-referencing truncate.
9. Reopen and verify FK metadata and immediate checks still work.

## Single-File And Embedded Lifecycle

No file-format change is required. FK metadata and rows remain in the primary
`.mylite` file. The slice adds close/reopen coverage so self-referential
metadata and row state are still discovered from the catalog.

## Storage-Engine Routing Impact

The behavior applies only to durable MyLite-routed base tables in the supported
FK subset, including omitted-engine and routed `ENGINE=InnoDB` tables.

## Public API, Wire-Protocol, Build, And Dependency Impact

No public C API, wire-protocol, dependency, or binary-size change is expected.
The coverage runs through existing direct SQL execution.

## Test And Verification Plan

- Add storage-smoke direct SQL coverage in
  `packages/libmylite/tests/embedded_storage_engine_test.c`.
- Run the MariaDB MyLite storage archive build if handler code changes.
- Run embedded storage/exec/statement smoke tests.
- Run default first-party format and storage checks.
- Run `git diff --check`.

## Acceptance Criteria

- Public self-referential `RESTRICT` / `NO ACTION` FK DDL succeeds for the
  documented durable routed table shape.
- Child inserts referencing existing parent rows succeed.
- Missing-parent and same-row self-reference inserts fail.
- Parent update/delete checks reject referenced parent rows and allow the same
  operations after child rows no longer reference them.
- Self-referencing truncate succeeds and leaves the table empty.
- Close/reopen preserves metadata and default checks-on behavior.

## Risks And Open Questions

- Same-row self-inserts are a real compatibility gap; supporting them probably
  requires statement-level ordering or deferred self-reference checks.
- Cascading self-references need recursion limits, cycle handling, prelocking,
  and transactional mutation semantics before MyLite can claim compatibility.
