# Foreign-Key Storage Metadata

## Goal

Add the first durable MyLite storage foundation for foreign-key definitions
without enabling public foreign-key SQL. The storage layer must be able to
publish, read, list, drop, rename, roll back, and recover FK metadata inside the
primary `.mylite` file so the next handler slice can expose MariaDB FK metadata
hooks without relying on InnoDB's dictionary or persistent sidecars.

## Non-Goals

- Accepting `CREATE TABLE` or `ALTER TABLE` foreign-key DDL through
  `libmylite`.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS` from the MyLite handler.
- Enforcing child/parent row checks or cascading actions.
- Designing `foreign_key_checks=0` import behavior.
- Reusing native InnoDB dictionary, redo, undo, or tablespace files.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_class.h:Foreign_key` stores parsed FK names, child columns,
  referenced schema/table/columns, match option, and update/delete actions.
- `mariadb/sql/table.h:FOREIGN_KEY_INFO` is the server-facing metadata shape
  used by `SHOW CREATE TABLE`, information schema, and FK-aware DDL checks.
- `mariadb/sql/handler.h` defines the FK handler hooks and
  `HTON_SUPPORTS_FOREIGN_KEYS`; MyLite must not advertise that bit until
  metadata hooks and enforcement agree.
- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` depends on child and
  parent FK metadata during copy ALTER.
- `mariadb/storage/innobase/handler/ha_innodb.cc` persists FK definitions in
  InnoDB-owned dictionary state, which conflicts with MyLite's single primary
  file and disabled-native-InnoDB profile.

## Compatibility Impact

Public compatibility remains unchanged: FK SQL is still rejected before MariaDB
execution. This slice adds internal first-party storage metadata only. The
compatibility matrix should describe it as groundwork, not as FK DDL or
referential-integrity support.

## Design

Add MyLite-owned FK metadata APIs to `packages/mylite-storage`:

- `mylite_storage_store_foreign_key_definition()`
- `mylite_storage_read_foreign_key_definition()`
- `mylite_storage_list_foreign_keys()`
- `mylite_storage_drop_foreign_key_definition()`
- `mylite_storage_free_foreign_key_metadata()`

The catalog gets a typed foreign-key record keyed by child schema/table and
constraint name. Parent schema/table are inline catalog fields so child and
parent table renames can update FK identity without rewriting metadata blobs.
Variable metadata lives in typed FK blob pages: referenced key name,
NUL-separated child and parent column names, update/delete actions, match
option, and child and referenced nullable-column bitmaps.

The storage API requires child and parent table records to exist before FK
metadata is stored. That is sufficient for this internal foundation; later SQL
DDL support can layer statement-scoped table-plus-FK publication on top of the
existing storage checkpoint machinery.

`DROP TABLE` removes child FK records and rejects dropping a referenced parent
while FK metadata remains. `RENAME TABLE` rewrites child FK identity and parent
references in catalog records while preserving table ids and metadata blob
pages.

## File Lifecycle

FK metadata is durable only inside the primary `.mylite` file. It uses the same
catalog root, page-count publication, rollback journal, transaction journal,
and recovery validation patterns as table-definition blob pages. Dropped or
rewritten FK metadata blobs may remain orphaned until the compaction slice, but
they are logically unreachable after catalog publication.

No `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, binlog,
relay-log, or plugin-owned durable file is introduced.

## Embedded Lifecycle And API

No public `libmylite` API changes are made in this slice. The new APIs are
first-party storage-layer primitives for future handler integration. Direct and
prepared FK DDL must continue to return the existing unsupported-surface
diagnostic.

## Build, Size, And Dependencies

No new dependency is introduced. The size impact is limited to first-party
storage catalog, encoding, decoding, and tests. The default embedded profile
continues to omit native InnoDB registration and dictionary code.

## Test Plan

- Storage unit coverage for FK capability reporting.
- Store/read/list coverage for FK metadata fields, column lists, and child and
  referenced nullable-column bitmaps.
- Duplicate FK metadata rejection.
- Parent and child table rename updates.
- Referenced-parent drop rejection.
- Explicit FK metadata drop.
- Child table drop cleanup followed by parent table drop.
- Format check, storage build, storage unit test, and `git diff --check`.

## Acceptance Criteria

- FK metadata records survive close/reopen through the existing storage read
  path.
- FK metadata remains in the primary `.mylite` file and uses typed FK blob
  pages.
- Child and parent table rename keep FK metadata logically reachable.
- Parent drops fail while a referencing FK record remains; child drops remove
  child FK records.
- Public FK SQL remains rejected until handler metadata hooks and row
  enforcement are implemented.

## Risks And Open Questions

- `CREATE TABLE` with inline FK clauses will need statement-scoped
  table-plus-FK publication before public FK DDL can be enabled.
- The nullable-column bitmaps support the current bounded internal shape; very
  wide FKs need a larger metadata representation if MariaDB compatibility
  requires them.
- Parent lookups are catalog scans. That is acceptable for the initial
  foundation but should be indexed or cached when FK enforcement becomes hot
  DML path.
