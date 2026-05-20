# Engine Support And Application Schemas

## Problem Statement

MyLite already proves controlled MyISAM, InnoDB transaction, and compatibility
query behavior. The next slice needs broader evidence that application
`ENGINE=` clauses route through MariaDB's native engine registry and that a
representative WordPress-shaped schema can be created, queried, closed, and
reopened inside the MyLite database directory.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc` resolves user engine names through
  `ha_resolve_by_name()` and rejects non-user-selectable engines. Disabled
  engines fall back through `ha_checktype()` only when substitution is allowed.
- `mariadb/sql/sys_vars.cc` exposes `default_storage_engine` as the session
  table plugin. `mariadb/sql/mysqld.cc` initializes the compiled default and
  MyLite currently overrides it with `--default-storage-engine=MyISAM`.
- `mariadb/sql/sql_table.cc` routes `CREATE TABLE` through
  `mysql_create_table()`, `mysql_create_table_no_lock()`, and
  `create_table_impl()`, where MariaDB checks engine options, builds the table
  path under `datadir`, writes metadata, and calls the native handler create
  path.
- `mariadb/storage/innobase/CMakeLists.txt`, `mariadb/storage/myisam/`,
  `mariadb/storage/maria/`, and `mariadb/storage/heap/` register InnoDB,
  MyISAM, Aria, and HEAP/MEMORY for the embedded build. The build cache keeps
  Blackhole, Archive, Federated, FederatedX, CONNECT, and Sphinx as dynamic
  plugin candidates, so they are not part of this default embedded slice.
- MariaDB documentation describes Aria as a native engine with `.MAI`, `.MAD`,
  and `aria_log*` files, and MEMORY as a table-definition-persistent engine
  whose row data lives in memory.
- WordPress application expectations are represented by common table shapes:
  `wp_options`, `wp_posts`, and `wp_postmeta` style InnoDB tables with
  unsigned `BIGINT`, `longtext`, prefixed indexes, zero DATETIME defaults, and
  `utf8mb4` charset/collation declarations.

## Proposed Design

Add a focused embedded compatibility test that:

1. Creates explicit `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and
   `ENGINE=MEMORY` tables in one schema.
2. Rejects optional dynamic-engine requests such as `ENGINE=BLACKHOLE` and
   `ENGINE=ARCHIVE` instead of silently falling back to another engine.
3. Creates a table without an explicit engine and verifies MariaDB resolves it
   to MyLite's current configured default engine, MyISAM.
4. Verifies resolved engines with `information_schema.TABLES`.
5. Inserts and queries representative rows before close.
6. Closes and reopens the database directory, then verifies durable engines
   kept rows while MEMORY kept the table definition but not rows.
7. Asserts durable native files for MyISAM and Aria remain under
   `datadir/<schema>/`, and that no unexpected file appears beside the MyLite
   database directory.
8. Creates and queries WordPress-shaped InnoDB tables in a separate schema.

The slice does not change the default engine. Keeping MyISAM as the temporary
default avoids a behavior change until a later storage-policy slice decides
whether the default should move to InnoDB or become configurable.

## Affected MariaDB Subsystems

- Storage-engine registry and plugin resolution.
- `CREATE TABLE` and native handler create paths.
- `information_schema.TABLES`.
- InnoDB, MyISAM, Aria, and HEAP/MEMORY embedded handlers.

## Compatibility Impact

The slice upgrades Aria and MEMORY from documented intent to tested partial
support in MyLite's embedded profile. It also backs representative
WordPress-shaped DDL with committed coverage. Dynamic external engines remain
out of scope for the default embedded profile and are covered as rejected
instead of silently falling back to another engine.

## DDL Metadata Routing Impact

DDL continues to use MariaDB's native `.frm` metadata and engine files inside
`datadir`. This slice adds evidence across multiple engines; it does not change
metadata format, catalog ownership, or DDL routing.

## Database-Directory And Lifecycle Impact

Durable files must remain inside the MyLite database directory. MEMORY table
rows are process-local and should not be treated as durable application state;
the table definition remains in `datadir`, while rows are empty after reopen.
Runtime and temporary directories must still be cleaned on close.

## Public API Impact

No public C API changes.

## Native Storage Impact

No custom storage layer is introduced. MyLite continues to rely on MariaDB
native handlers and tests the observable file layout for supported engines.

## Wire-Protocol Or Integration Impact

None for this slice.

## Binary-Size Impact

No build-profile changes are planned. Existing embedded-engine availability is
tested as-is, and size-profile hardening remains a later slice.

## License Or Dependency Impact

No new dependencies or license changes.

## Test And Verification Plan

- Add `libmylite.embedded-engine-schema` under `embedded-dev`.
- Label it `compat.engine`, `compat.application-query`,
  `compat.directory-boundary`, and `compat.query`.
- Run:
  - `cmake --build --preset dev`
  - `ctest --preset dev --output-on-failure`
  - `cmake --build --preset embedded-dev`
  - `ctest --preset embedded-dev --output-on-failure`
  - `ctest --preset embedded-dev -L compat.engine --output-on-failure`
  - `cmake --build --preset embedded-dev --target format-check`
  - `cmake --build --preset dev --target tidy`
  - `cmake --build --preset embedded-dev --target tidy`
  - `git diff --check`
  - `tools/mariadb-embedded-build measure`

## Acceptance Criteria

- Supported engine availability is asserted by successful explicit table
  creation and MariaDB table metadata.
- Explicit InnoDB, MyISAM, Aria, MEMORY, and default-engine table creation is
  covered.
- Unsupported optional dynamic engines are rejected explicitly.
- Durable engines survive close and reopen with rows intact.
- MEMORY survives close and reopen as a definition with empty row state.
- WordPress-shaped InnoDB DDL and representative queries pass.
- Compatibility and roadmap docs reflect the implemented coverage without
  claiming dynamic plugin support or size-profile work.

## Risks And Unresolved Questions

- MyLite's current default engine is still temporary MyISAM. This slice records
  and tests that behavior rather than changing it.
- Blackhole and Archive are rejected in the current default profile because the
  embedded build leaves them as dynamic plugin candidates. Supporting them needs
  a dedicated profile decision.
- WordPress coverage is representative, not a claim that every WordPress core
  table or plugin migration works.
