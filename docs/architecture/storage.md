# Storage Architecture

MyLite storage is a purpose-built MariaDB storage engine backed by one primary
`.mylite` file. MariaDB provides SQL parsing, metadata semantics, optimizer
integration, expression evaluation, diagnostics, and handler calls; MyLite owns
durable catalog, row, index, transaction, lock, and recovery state.

## Product Invariant

"Single file" means one primary database asset, not "no other file is ever
created while the database is open."

- The portable durable asset is one file, such as `app.mylite`.
- Persistent MariaDB sidecars are not valid MyLite storage: no `.frm`, `.ibd`,
  `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, `ib_logfile*`, binlog, relay
  log, or plugin-owned durable table files.
- MyLite-owned rollback journal, WAL, shared-memory, lock, and temporary spill
  files are allowed when their names, recovery behavior, cleanup behavior, and
  failure modes are documented and tested.
- Recovery companions left after an unclean shutdown are part of the MyLite
  lifecycle, not separate user-managed database assets.

This is stricter than a datadir-in-a-directory model and more practical than a
rule that forbids recovery or temporary companions.

## Architecture Decision

MyLite uses a new static storage engine. It does not wrap an ordinary MariaDB
datadir inside a container file, and it does not put SQLite SQL execution below
MariaDB SQL execution.

Reasons:

- MariaDB's handler API is the correct boundary for preserving MariaDB SQL
  semantics while replacing durable storage.
- Existing InnoDB, MyISAM, and Aria durable files conflict with the portable
  primary-file model.
- A virtual datadir keeps MariaDB's file, log, lock, and recovery systems
  nested inside another filesystem layer; that is harder to make durable and
  smaller than a direct storage engine.
- SQLite is useful as design evidence for file ownership and pager tradeoffs,
  but SQLite SQL semantics do not match MariaDB semantics.

## Implementation Boundary

Durable MyLite storage lives in the internal first-party
`packages/mylite-storage/` target. MariaDB-facing handler glue lives under
`mariadb/storage/mylite/` and should stay as thin as practical: translate
MariaDB handler calls into MyLite storage operations, return MariaDB handler
errors, and preserve upstream registration conventions.

This split keeps catalog, page, transaction, lock, and recovery code outside the
MariaDB import while limiting the long-lived fork delta under `mariadb/`.

The initial handler is an opt-in registration skeleton. It is disabled in the
default embedded baseline and covered by a separate storage smoke build that
verifies `SHOW ENGINES` reports `MYLITE`. It deliberately rejects table creation
until the catalog, row storage, and DDL routing slices define real behavior.

## File Layout

The `.mylite` file format should be page based:

```text
header
catalog pages
table row pages
index pages
transaction metadata
free-space metadata
integrity and checkpoint metadata
```

The header stores:

- magic bytes,
- file-format version,
- MyLite library compatibility version,
- page size,
- endian marker,
- checksum mode,
- catalog root page,
- transaction/checkpoint pointers,
- durability and feature flags.

The catalog stores:

- schemas,
- table definitions,
- table-definition binary images needed by MariaDB discovery,
- columns, indexes, constraints, and engine metadata,
- views, triggers, and routines when those surfaces are supported,
- collation and character-set metadata needed to reopen tables,
- autoincrement state,
- table and index root pages.

## Table Definitions

The first metadata bridge stores MariaDB-produced table-definition images in
the MyLite catalog. MariaDB's table-discovery API can initialize a
`TABLE_SHARE` from a binary `.frm` image or from a SQL statement string; the
binary image is the lower-risk first bridge because it preserves the exact
definition MariaDB produced.

DDL routing must cover both discovery and writes:

1. Let MariaDB build the canonical table definition during `CREATE` or `ALTER`.
2. Store the generated definition and MyLite metadata in the catalog.
3. Suppress durable `.frm` creation for MyLite tables.
4. Implement `discover_table()`, `discover_table_names()`, and
   `discover_table_existence()` from the catalog.
5. Use catalog table-definition versions to detect stale cached definitions and
   report `HA_ERR_TABLE_DEF_CHANGED` when required.

`CREATE`, `ALTER`, `DROP`, and `RENAME` are the minimum DDL lifecycle for
claiming native single-file table metadata.

## Schemas And System Surfaces

MariaDB's `database.table` model maps to catalog namespaces:

```text
schema_id -> schema name
table_id  -> schema_id + table name
index_id  -> table_id + index name
```

No persistent directory is created for a schema. `CREATE DATABASE`,
`DROP DATABASE`, `USE`, table-name resolution, and information schema listing
read catalog namespaces.

The default embedded profile does not expose server account administration,
dynamic plugin installation, replication metadata, or the event scheduler.
`information_schema` remains virtual. Any required `mysql.*` system surface
must be implemented as MyLite-backed metadata or a read-only virtual surface,
not as Aria tables in a datadir.

## Rows And Indexes

Rows and indexes live in MyLite pages, not in MariaDB engine files. The first
row format should preserve enough MariaDB record layout information to avoid
inventing a parallel SQL type system. Over time, the storage format can move
toward typed native encodings when there is a compatibility and size benefit.

The storage engine must support:

- table scans,
- primary, unique, and secondary indexes,
- ordered index reads,
- duplicate-key checks,
- nullable-key semantics,
- BLOB/TEXT overflow storage,
- autoincrement state,
- row insert/update/delete,
- truncate,
- table rebuilds for copy `ALTER`.

FULLTEXT, SPATIAL, generated-column indexes, and foreign-key enforcement need
explicit storage designs before support is claimed.

## Transactions And Recovery

MyLite needs its own transaction and recovery layer. It must not rely on
InnoDB, Aria, binlog, or server datadir recovery as durable application state.

Minimum guarantees:

- atomic publication of catalog, row, and index changes,
- rollback for failed statements and transactions,
- crash recovery after process or OS crash,
- corruption detection for critical pages,
- deterministic recovery and cleanup for any MyLite-owned journal or WAL
  companions,
- file locking that prevents unsafe concurrent writers.

The storage design must preserve the full write-concurrency goal. Early
milestones may use coarse locks for correctness, but the page, transaction,
and lock manager designs must not bake in single-writer-only assumptions.

## Temporary Data

Temporary tables, query spill files, and recovery companions are storage policy,
not violations of the primary-file model.

- User temporary tables start as session-local state and do not publish durable
  catalog entries.
- Internal temporary spill may use MyLite-owned temporary files.
- Strict no-temp-file modes may exist, but they trade off query limits and
  performance.
- Companion files must use deterministic names tied to the primary file and
  must be covered by lifecycle tests.

## Migration

MyLite does not open arbitrary MariaDB datadirs as `.mylite` files. Migration is
logical first:

- import SQL dumps,
- export MariaDB-compatible SQL dumps where practical,
- add stopped-datadir import tooling only after the storage engine is stable.

## Source References

- MariaDB embedded interface: <https://mariadb.com/kb/en/embedded-mariadb-interface/>
- MariaDB table discovery: <https://mariadb.com/kb/en/table-discovery/>
- Aria storage and log files: <https://mariadb.com/docs/server/server-usage/storage-engines/aria/aria-storage-engine>
- InnoDB tablespaces: <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/innodb-tablespaces/innodb-file-per-table-tablespaces>
