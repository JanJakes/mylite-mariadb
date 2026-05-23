# Default Engine, Memory Databases, And Broader DDL Coverage

## Goal

Align MyLite's implicit table engine with MariaDB's compiled default, expand
coverage for the special `:memory:` database path, and add compatibility
coverage for common DDL forms that real applications use beyond simple
`CREATE TABLE`, `ALTER TABLE ADD COLUMN`, `RENAME TABLE`, and `DROP TABLE`.

## Non-Goals

- Do not introduce a MyLite-specific default engine policy.
- Do not replace MariaDB native storage or add a custom storage engine.
- Do not claim read-only opens, shared readers, or concurrent writers for this
  slice.
- Do not make `:memory:` a durable database path or a no-files-at-all storage
  mode; it remains a transient MyLite-owned runtime layout.
- Do not complete every DDL feature. Views, triggers, stored functions, online
  DDL algorithms, partition DDL, and metadata edge cases remain later work.
- Do not change binary-size policy.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/mysqld.cc` initializes `default_storage_engine` to `InnoDB`
  when `WITH_INNOBASE_STORAGE_ENGINE` is defined, and to `MyISAM` otherwise.
- `mariadb/sql/sys_vars.cc` exposes `@@default_storage_engine` as the session
  table plugin backed by `default_storage_engine`.
- `mariadb/sql/handler.cc` resolves omitted `ENGINE=` clauses through
  `ha_default_plugin()` / `ha_default_handlerton()`.
- `mariadb/sql/sql_table.cc` routes `CREATE TABLE ... LIKE`, `CREATE TABLE ...
  SELECT`, standalone index DDL, generated columns, checks, and foreign keys
  through the same native metadata and handler paths used by ordinary
  `CREATE TABLE` and `ALTER TABLE`.
- Before this slice, MyLite passed `--default-storage-engine=MyISAM` during
  embedded startup, which overrode the MariaDB default and made no-engine DDL
  diverge from the selected MariaDB build.
- MyLite's `:memory:` path creates a unique transient runtime directory below
  `mylite_open_config.temp_directory` or the host temp directory, points MariaDB
  data, temporary, and plugin paths inside it, and removes it on close.

## Compatibility Impact

Tables created without `ENGINE=` now follow the MariaDB build default. In the
current embedded profile, that means InnoDB because InnoDB is built in. This is
closer to modern MySQL/MariaDB application expectations and avoids a
MyLite-only MyISAM policy.

`docs/COMPATIBILITY.md` moves the default-engine row from temporary MyISAM to
MariaDB-default InnoDB-for-this-profile coverage, moves selected common DDL
forms from planned to partial coverage, and describes `:memory:` as transient
runtime-directory backed.

## Design

Remove MyLite's `--default-storage-engine=MyISAM` startup option and let
MariaDB initialize the default engine. Keep all existing explicit engine
coverage; default-engine tests should query resolved table metadata instead of
assuming a MyLite policy.

Add a focused `:memory:` embedded test that opens the special path, exercises
direct SQL and prepared statements over transient tables, verifies transaction
behavior for the current default engine, verifies explicit supported engines,
and checks that close removes the transient runtime tree. Reopening `:memory:`
starts a fresh empty database.

Add a broader DDL embedded test over the durable `.mylite/` layout. It should
cover:

- `CREATE TABLE` without `ENGINE=`,
- `CREATE TABLE ... LIKE`,
- `CREATE TABLE ... SELECT`,
- standalone `CREATE INDEX` and `DROP INDEX`,
- representative `ALTER TABLE` column and index changes,
- CHECK constraint enforcement,
- InnoDB foreign-key enforcement,
- generated stored and virtual columns.

## File Lifecycle

Durable database paths continue to keep native MariaDB state under:

```text
<name>.mylite/
  mylite.meta
  mylite.lock
  datadir/
  tmp/
```

Clean close removes `run/` and clears `tmp/`. Default-engine InnoDB files,
metadata, and DDL side effects must stay under `datadir/`.

For `:memory:`, MyLite creates an implementation-owned transient runtime
directory such as:

```text
<temp>/mylite-runtime-<id>/
  data/
  tmp/
  plugins/
```

That directory is removed on close. Callers must not rely on its name or on
files surviving the handle lifetime.

## Embedded Lifecycle And API

No public C API changes. `mylite_open(":memory:", ...)` keeps using the normal
open flags and the existing SQLite-like special path convention. The expanded
coverage should prove direct execution, prepared statements, transactions, and
clean close work through the same handle lifecycle as durable databases.

## Build, Size, And Dependencies

No new dependencies. No size-profile changes are expected beyond test binaries.

## Test Plan

1. Update `libmylite.embedded-engine-schema` to expect MariaDB's resolved
   default engine instead of MyISAM.
2. Add `libmylite.embedded-memory-database` for `:memory:` direct SQL,
   prepared statements, transaction behavior, explicit engines, close cleanup,
   and fresh reopen behavior.
3. Add `libmylite.embedded-ddl-coverage` for broader durable DDL forms.
4. Run focused embedded tests for default-engine, `:memory:`, DDL, prepared
   statements, transactions, and directory-boundary labels.
5. Run the standard build, format, static-analysis, and diff checks used by
   current MyLite slices.

## Acceptance Criteria

- MyLite no longer passes a MyISAM-specific default-engine startup option.
- `@@default_storage_engine` and no-engine table metadata resolve to InnoDB in
  the current embedded profile.
- `:memory:` has explicit coverage for SQL, prepared statements, transactions,
  supported engines, cleanup after close, and fresh database state on reopen.
- Broader DDL coverage passes for table-copy creation, CTAS, standalone index
  DDL, representative `ALTER TABLE`, CHECK constraints, foreign keys, and
  generated columns.
- Compatibility, API, architecture, and roadmap docs describe the updated
  behavior without claiming unsolved concurrency or no-file memory semantics.

## Risks And Open Questions

- The selected MariaDB build determines the implicit engine. If a future build
  omits InnoDB, default-engine expectations must follow that build rather than
  hard-code InnoDB.
- `:memory:` is currently transient-directory backed. A true RAM-only native
  storage profile would need a separate design because InnoDB, MyISAM, and Aria
  normally create engine files.
- DDL support remains broad but not exhaustive. Online DDL algorithms, views,
  triggers, routines, partitioning, and metadata compatibility need separate
  coverage before they can be claimed.
