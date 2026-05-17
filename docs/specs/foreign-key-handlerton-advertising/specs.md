# Foreign-Key Handlerton Advertising

## Goal

Advertise MyLite's covered foreign-key subset through
`HTON_SUPPORTS_FOREIGN_KEYS` now that public FK DDL, handler metadata,
immediate `RESTRICT` / `NO ACTION` row checks, `foreign_key_checks=0` row-DML
bypass, `DROP FOREIGN KEY`, and generated supporting-key cleanup are
implemented.

The slice must keep MyLite's claim narrow: advertising the handlerton bit does
not mean full InnoDB foreign-key compatibility. Cascades, `SET NULL`,
`SET DEFAULT`, partitioned-table FKs, volatile zero-file FKs, and broader
transaction-aware FK behavior remain planned.

## Non-Goals

- Implementing cascading actions or `SET NULL` / `SET DEFAULT`.
- Making FK checks deferrable.
- Adding FK support for temporary, `MEMORY`, `HEAP`, or `BLACKHOLE` tables.
- Reintroducing InnoDB dictionary, lock manager, or persistent sidecar files.
- Changing the public `libmylite` C API.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h` defines `HTON_SUPPORTS_FOREIGN_KEYS` as the storage
  engine's FK support advertisement bit, alongside the handler FK metadata
  hooks `get_foreign_key_list()`, `get_parent_foreign_key_list()`, and
  `referenced_by_foreign_key()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_init()` sets
  `HTON_SUPPORTS_FOREIGN_KEYS` together with InnoDB's other handlerton flags.
- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` always asks the
  handler for retained child and parent FK metadata during copy `ALTER`, but
  skips SQL-layer supporting-key validation when
  `table->file->ht->flags & HTON_SUPPORTS_FOREIGN_KEYS` is true. Once MyLite
  sets the bit, retained FK key validation must be owned by the MyLite handler.
- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` still performs
  column drop, rename, and significant type-change checks for retained child
  and parent FK columns regardless of the handlerton bit.
- `mariadb/sql/sql_truncate.cc` rejects `TRUNCATE TABLE` on parent tables by
  using `referenced_by_foreign_key()` and `get_parent_foreign_key_list()`.
- `mariadb/sql/sql_base.cc:prepare_fk_prelocking_list()` uses parent FK
  metadata to add FK child tables to DML prelocking sets. For the current
  MyLite subset this mostly affects `RESTRICT` / `NO ACTION` parent row
  updates and deletes; future cascade actions will need a separate lock and row
  mutation design.
- `mariadb/storage/mylite/ha_mylite.cc` already implements the handler FK
  metadata hooks over MyLite catalog records, validates newly added FK shapes,
  enforces immediate child and parent row checks, and exposes FK text through
  `SHOW CREATE TABLE`.

## Compatibility Impact

The compatibility claim changes from "FK metadata and enforcement exist but the
engine does not advertise FK support" to "MyLite advertises engine FK support
for its documented subset." The advertised subset is still narrower than
InnoDB:

- supported: durable MyLite-routed tables, including omitted engine and routed
  `ENGINE=InnoDB`; validated `RESTRICT` / `NO ACTION` FKs over supported child
  key prefixes and exact unique parent keys; immediate row checks; metadata
  visibility; FK-aware copy ALTER key retention; `foreign_key_checks=0` row-DML
  bypass;
- unsupported/planned: cascades, `SET NULL`, `SET DEFAULT`, partitioned FKs,
  volatile or row-discarding engines, deferrable checks, and broader
  transaction-aware FK integration.

## Design

Set `HTON_SUPPORTS_FOREIGN_KEYS` in the MyLite handlerton only after adding
handler-side retained-FK key validation.

The validation runs from `ha_mylite::create()` during copy `ALTER`, before the
rebuilt table definition is stored:

1. For retained child FKs on the logical table, ignore FK records explicitly
   dropped by the same `ALTER TABLE ... DROP FOREIGN KEY` statement. Every
   remaining child FK must still have a supported key prefix over the child
   columns in the rebuilt `TABLE` object.
2. For retained parent FKs that reference the logical table, ignore only
   self-referencing FK records explicitly dropped by the same `ALTER TABLE`.
   Every remaining parent FK must still have the stored referenced key name as
   an exact supported unique key over the referenced columns in the rebuilt
   `TABLE` object.
3. Continue relying on MariaDB's existing copy-ALTER checks for retained FK
   column drops, renames, and significant type changes. MyLite's new handler
   validation covers the supporting-key checks that MariaDB delegates to the
   engine once the handlerton flag is set.
4. Keep generated supporting-key cleanup behavior intact: dropping a generated
   child key is allowed only when the rebuilt table contains another supported
   child key prefix for the retained FK.

## Affected Subsystems

- `mariadb/storage/mylite/`: handlerton flags and retained-FK key validation.
- `packages/libmylite/tests/`: embedded storage-engine coverage for copy ALTER
  and FK metadata behavior.
- Compatibility and architecture docs that describe FK support status.

## Single-File And Embedded Lifecycle

No new file or durable sidecar is introduced. Validation reads retained FK
records from the primary `.mylite` catalog and inspects the rebuilt in-memory
`TABLE` object. Existing MyLite statement checkpoints continue to protect copy
ALTER rollback.

No public API or embedded lifetime rule changes. The flag affects MariaDB SQL
layer behavior inside the embedded runtime only.

## Storage-Engine Routing Impact

The bit advertises support for MyLite's effective storage engine, including
routed omitted-engine and `ENGINE=InnoDB` tables. Native InnoDB remains absent
from the embedded profile, and unsupported engines continue to fail before
catalog publication.

## Wire-Protocol And Integration Impact

No wire-protocol code changes are required. Future wire-protocol packages will
inherit the same SQL behavior through `libmylite`.

## Binary-Size, License, And Dependency Impact

No dependency is added. The MariaDB fork delta stays inside MyLite's storage
handler. Binary-size impact should be negligible: one handlerton flag and a
small amount of handler validation code.

## Test And Verification Plan

- Storage-smoke coverage that existing public and catalog-seeded FK tables
  still reject dropping retained child supporting keys.
- Storage-smoke coverage that existing public and catalog-seeded FK parent
  tables still reject dropping retained exact unique parent keys.
- Storage-smoke coverage that generated child supporting keys can still be
  removed when another supported child key prefix exists.
- Storage-smoke coverage that `TRUNCATE TABLE` remains rejected for referenced
  parent tables.
- Build the MariaDB MyLite storage archive, run embedded storage/exec/statement
  smoke tests, run default storage tests and format checks, and run
  `git diff --check`.

## Acceptance Criteria

- MyLite sets `HTON_SUPPORTS_FOREIGN_KEYS`.
- Retained child FKs cannot lose their last supported child key prefix during
  copy `ALTER`, even though MariaDB now delegates FK key validation to the
  engine.
- Retained parent FKs cannot lose their stored referenced exact unique key
  during copy `ALTER`.
- Existing generated supporting-key cleanup still succeeds when a replacement
  supported child key exists.
- Docs describe the advertised subset without implying full InnoDB FK support.

## Risks And Open Questions

- Prelocking now has a stronger signal that MyLite has FK metadata. Current
  `RESTRICT` / `NO ACTION` row checks are immediate and do not mutate child
  tables, but cascades will need a separate prelocking, recursion, and
  transaction design.
- Parent-key validation intentionally requires the stored referenced key name
  to remain present. That matches the current metadata model and avoids silent
  FK retargeting during copy `ALTER`.
- This slice does not change transaction isolation or cross-process write
  concurrency guarantees for FK checks; those remain part of later storage and
  transaction work.
