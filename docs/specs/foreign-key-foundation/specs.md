# Foreign-Key Foundation

## Goal

Define the first safe implementation boundary for MyLite foreign keys before
removing the current explicit FK DDL rejection. The implementation must preserve
the product shape: routed `ENGINE=InnoDB` and default-MyLite tables may not
accept FK metadata until MyLite can persist it in the primary `.mylite` file,
expose it through MariaDB's handler metadata hooks, and enforce the covered row
rules.

## Non-Goals

- Enabling FK DDL in this design-only slice.
- Implementing cascading actions, deferred checks, or partition-aware FKs.
- Supporting foreign keys over unsupported index classes, virtual generated
  columns, external engines, or cross-file references.
- Treating `foreign_key_checks=0` as a bypass before dump-import semantics are
  specified and tested.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses table-level `FOREIGN KEY` and column-level
  `REFERENCES` clauses into `Foreign_key` entries through
  `LEX::add_table_foreign_key()` and `LEX::add_column_foreign_key()`.
- `mariadb/sql/sql_class.h:Foreign_key` stores the parsed constraint name,
  referenced schema/table, child columns, referenced columns, match option, and
  update/delete actions.
- `mariadb/sql/sql_class.cc:Foreign_key::validate()` validates child column
  existence and rejects some invalid generated-column action combinations.
- `mariadb/sql/handler.h` uses `HTON_SUPPORTS_FOREIGN_KEYS` as the storage
  engine capability bit and defines the handler hooks MyLite must implement:
  `get_foreign_key_create_info()`, `get_foreign_key_list()`,
  `get_parent_foreign_key_list()`, `referenced_by_foreign_key()`, and
  `is_fk_defined_on_table_or_index()`.
- `mariadb/sql/table.h:FOREIGN_KEY_INFO` is the server-facing metadata shape
  returned by those hooks: child id/schema/table, parent schema/table, child
  columns, referenced columns, referenced key name, nullable bits, and
  update/delete methods.
- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` calls child and
  parent FK list hooks before copy `ALTER` and rejects column changes/drops
  that could break FK integrity.
- `mariadb/sql/sql_base.cc` and `mariadb/sql/sql_truncate.cc` consult
  `referenced_by_foreign_key()` plus parent-key metadata to protect operations
  that delete or truncate parent rows while FK checks are enabled.
- `mariadb/sql/sql_show.cc` calls FK metadata hooks for `SHOW CREATE TABLE` and
  information-schema FK surfaces.
- `mariadb/storage/innobase/handler/ha_innodb.cc` advertises
  `HTON_SUPPORTS_FOREIGN_KEYS`, persists FK metadata in InnoDB's dictionary,
  prints FK create clauses, and populates `FOREIGN_KEY_INFO` lists. MyLite
  cannot reuse InnoDB's dictionary because native InnoDB is disabled in the
  embedded profile and persistent InnoDB sidecars violate the single-file
  invariant.

## Compatibility Impact

Foreign keys remain partial and explicitly rejected until implementation
reaches the acceptance criteria below. The first user-visible support claim
should be deliberately smaller than InnoDB:

- same primary `.mylite` file only;
- existing parent and child base tables routed to MyLite;
- child and parent columns backed by supported MyLite key shapes;
- immediate `RESTRICT` / `NO ACTION` checks for child insert/update and parent
  update/delete;
- metadata exposed through `SHOW CREATE TABLE`, `SHOW INDEX`, and relevant
  information-schema surfaces.

`ON UPDATE CASCADE`, `ON DELETE CASCADE`, `SET NULL`, `SET DEFAULT`, deferrable
checks, partitioned tables, virtual generated-column edge cases, and
`foreign_key_checks=0` import bypass remain separate slices.

## Design

Keep the current `libmylite` SQL rejection while building the foundation in
three implementation steps.

1. Add first-party storage metadata for FK definitions.
   - Persist FK records in the primary `.mylite` file, keyed by child
     schema/table and constraint name.
   - Store parent schema/table, child columns, parent columns, referenced key
     name, match option, update/delete actions, and nullable bits needed to
     build `FOREIGN_KEY_INFO`.
   - Make FK records participate in catalog checkpoint, rollback, recovery,
     drop, rename, and close/reopen tests.

2. Add handler metadata hooks without enabling public FK DDL yet.
   - Implement internal helpers that can construct `FOREIGN_KEY_INFO` lists
     from MyLite catalog records.
   - Implement `referenced_by_foreign_key()` and
     `is_fk_defined_on_table_or_index()` over catalog metadata.
   - Add tests through first-party helpers or guarded handler fixtures before
     setting `HTON_SUPPORTS_FOREIGN_KEYS`.

3. Enable a narrow `RESTRICT` / `NO ACTION` FK subset.
   - Remove the public SQL rejection only for supported FK DDL shapes.
   - Advertise `HTON_SUPPORTS_FOREIGN_KEYS` when the handler can persist
     metadata and enforce the covered row checks.
   - Validate parent table existence, referenced key existence, column count,
     type/collation compatibility, and supported key shapes before catalog
     publication.
   - Enforce child insert/update parent existence and parent update/delete
     absence of referencing child rows through MyLite index lookups.
   - Keep unsupported actions and unsupported table/index shapes rejected with
     stable MyLite diagnostics before catalog publication.

## File Lifecycle

FK metadata must live in the primary `.mylite` file and participate in the same
statement checkpoints as table definitions, rows, indexes, and autoincrement
state. Failed FK DDL must leave no FK record, no partial table definition, and
no durable MariaDB `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`,
`aria_log.*`, binlog, relay-log, or plugin-owned table files.

Parent/child drop, rename, and copy `ALTER` must update or reject based on FK
metadata before publishing new catalog roots. Old orphaned pages can remain
uncompacted until the compaction slice, but logical FK visibility must be
restored by rollback and recovery.

## Embedded Lifecycle And API

No public API addition is required for the first FK support slice. Existing
`mylite_exec()`, prepared statement, warning, and diagnostic APIs should expose
the same SQL behavior applications expect from MariaDB for the covered subset.

Before FK support is enabled, direct and prepared FK DDL must continue to
return the current stable MyLite unsupported-surface diagnostic.

## Build, Size, And Dependencies

No new dependency is needed. The implementation will add first-party storage
metadata code and MariaDB handler glue. It must not re-enable native InnoDB FK
dictionary code or persistent InnoDB sidecars in the embedded profile.

## Test Plan

- Storage-unit tests for FK catalog record publish, read, drop, rename,
  checkpoint rollback, and corrupt-record rejection.
- Storage-smoke tests that keep current FK DDL rejection until the supported
  subset is ready.
- Handler metadata tests for child/parent FK lists, referenced-parent checks,
  `SHOW CREATE TABLE`, and information-schema rows.
- Narrow support tests for:
  - parent/child `CREATE TABLE` with explicit `ENGINE=InnoDB` routed to MyLite;
  - child insert/update rejection when the parent key is missing;
  - parent update/delete rejection when child rows reference it;
  - close/reopen metadata and enforcement;
  - failed FK DDL rollback and sidecar gates;
  - unsupported cascades/actions and unsupported key shapes.
- Compatibility harness coverage under the existing `foreign-key` group.
- Full `dev`, `embedded-dev`, and `storage-smoke-dev` presets, format, tidy,
  shell checks, and `git diff --check`.

## Acceptance Criteria

- FK DDL remains rejected until metadata hooks and row enforcement for the
  covered subset are implemented together.
- Supported FK metadata is durable inside one `.mylite` file and visible after
  close/reopen through MariaDB metadata surfaces.
- Covered child and parent DML checks are enforced before and after reopen.
- Unsupported actions and unsupported table/index shapes fail before catalog
  publication.
- Failed FK DDL and failed FK-enforced DML restore catalog, row, index, and FK
  visibility through existing statement rollback mechanisms.
- Docs clearly distinguish the covered FK subset from full InnoDB FK semantics.

## Risks And Open Questions

- MariaDB's FK DDL and copy `ALTER` paths rely heavily on handler metadata
  hooks. Enabling `HTON_SUPPORTS_FOREIGN_KEYS` too early would create false
  compatibility claims and may break unrelated ALTER/DROP/TRUNCATE behavior.
- Efficient enforcement needs index lookups over parent and child keys; a
  table-scan fallback would be simple but could become unacceptable for common
  ORM workloads.
- `foreign_key_checks=0` is common in dumps. MyLite should design that import
  behavior explicitly instead of inheriting accidental MariaDB session-variable
  behavior.
