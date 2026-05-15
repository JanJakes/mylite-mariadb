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

The initial handler is opt-in. It is disabled in the default embedded baseline
and covered by a separate storage smoke build. That build verifies the
`MYLITE` row from `SHOW ENGINES`, explicit `CREATE TABLE ... ENGINE=MYLITE`
stores metadata in the primary `.mylite` catalog, and catalog discovery works
after close/reopen. Routed tables support row inserts, full scans, updates, and
deletes from the primary file, including BLOB/TEXT payloads. Supported
primary, unique, and secondary indexes append durable index-entry pages and
serve ordered handler cursors from MariaDB key tuples. `DROP TABLE` removes
catalog metadata for routed tables. Simple `RENAME TABLE` updates catalog
identity while preserving table ids, row pages, and index-entry pages. Copy
`ALTER` rebuilds use MariaDB's table-copy path and append rebuilt table
definitions, rows, and supported index entries inside the primary file. Online
`ALTER`, in-place `ALTER`, transaction-aware index maintenance, truncate,
free-space reclamation, and unsupported index classes still reject or remain
planned until those slices define the paths. Standalone `CREATE INDEX` and
`DROP INDEX` use MariaDB's ALTER-backed DDL path for supported copy-rebuild
index additions and drops.

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

The current implementation writes page 0 as a fixed-size, little-endian,
checksummed header and page 1 as a catalog root. Explicit MyLite table
definitions are stored as catalog records plus checksummed definition blob
pages. Row inserts append checksummed row pages tagged by catalog table id;
non-BLOB rows store raw MariaDB record images, while BLOB/TEXT rows store a
durable handler-owned row payload that replaces process pointers with value
bytes. Large row payloads spill into checksummed row-payload blob pages inside
the primary file. Update/delete appends checksummed row-state pages that hide
deleted or superseded row page ids; replacement row payloads are appended as
new row pages. Table scans validate those pages, filter hidden row ids, and
reconstruct MariaDB row buffers before returning them to the SQL layer.
Autoincrement tables append checksummed state pages keyed by catalog table id so
generated values survive close/reopen and dropped table ids do not leak into
recreated tables. Supported primary, unique, and secondary indexes append
checksummed index-entry pages containing the catalog table id, MariaDB key
number, row page id, and MariaDB key-tuple bytes. Handler index reads build
ordered in-memory cursors from live index entries and compare keys with
MariaDB's key helpers. Current mutating publication paths are protected by a
rollback journal before the header or catalog root page is overwritten.
Transaction state, free-space metadata, and B-tree-style index navigation are
still planned slices.

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

Current support covers metadata capture and discovery for omitted/default
engine requests, explicit `ENGINE=MYLITE`, and metadata-safe `ENGINE=InnoDB`,
`ENGINE=MyISAM`, and `ENGINE=Aria` requests. The catalog records both the
requested engine name and the effective `MYLITE` engine. Unsupported explicit
engine requests fail before catalog publication. `DROP TABLE` removes the live
catalog record and increments the catalog generation without deleting external
MariaDB sidecars. Dropped table-definition blobs, row pages, and index-entry
pages remain orphaned inside the primary file until free-space management
exists; new table ids are allocated above both live catalog records and
existing row pages so drop/recreate does not expose old rows. Simple
`RENAME TABLE` rewrites the catalog record identity while preserving table id,
requested/effective engine metadata, and the stored table-definition blob
reference, so existing row and index-entry pages move with the renamed table.
Copy `ALTER` rebuilds let MariaDB create a temporary MyLite table, copy rows
through `ha_write_row()`, rename the old table to a backup, rename the rebuilt
table to the final name, and drop the backup catalog record. This preserves
requested engine metadata for implicit rebuilds and records explicit supported
engine requests on engine rebuilds.
Supported key additions on copy `ALTER` rebuild through the same table-copy
path and publish rebuilt rows with matching index-entry pages. `LOCK=NONE` copy
ALTER, in-place ALTER, unsupported index rebuilds, and transactional DDL
rollback remain planned until MyLite has locking and recovery.

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

Current row support is append-only. `write_row()` stores a durable MyLite row
payload in a row page, `rnd_next()` reads those payloads back during full table
scans, `position()` stores the row page id as the handler row reference, and
`rnd_pos()` reopens a live row by that id for sorted update/delete paths.
`update_row()` appends a replacement row, a row-state page that hides the old
row id, and replacement index entries for supported keys. `delete_row()`
appends a row-state page that hides the current row id; stale index entries
remain on disk until compaction exists but are filtered through the row-state
map. Nullable fixed and variable fields are covered because the stored record
image includes MariaDB's null bitmap. BLOB/TEXT fields are serialized as
length-prefixed value bytes, not process pointers, and large payloads use
primary-file overflow pages.

Supported primary, unique, and secondary keys use MariaDB key tuples generated
from the row buffer. The handler rejects unsupported key classes before table
publication, including FULLTEXT, SPATIAL, generated, hash, and BLOB/TEXT-prefix
keys. Duplicate checks read live index entries, use MariaDB key comparison, and
preserve nullable unique-key semantics. Ordered index reads build in-memory
cursors from live index entries and then reconstruct row buffers from row
pages. This provides correct indexed insert, lookup, update, delete, reopen,
and copy `ALTER` behavior for the supported shapes, but it is not the final
performance structure. Standalone `CREATE INDEX` and `DROP INDEX` are covered
for supported copy-rebuild index definitions. Truncate, B-tree pages,
free-space reclamation, transaction rollback, and transaction-aware index
maintenance remain planned.

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

Current implementation status: MyLite writes a deterministic
`<database>.mylite-journal` rollback companion before publishing current
append-only mutations. The journal stores the committed header page and, for
catalog mutations, the committed catalog root page. It is fsynced before
primary-file writes, the primary file is fsynced before the journal is removed,
the parent directory is synced after journal create/remove, and every storage
open first recovers and removes a valid pending journal. Recovery restores the
previously committed header/catalog state; appended pages beyond the restored
header page count are ignored until free-space reclamation exists.

Storage operations now use non-blocking advisory locks on the primary
`.mylite` file descriptor. Read APIs take a shared lock after pending recovery
is handled, while writes and recovery take an exclusive lock before mutating or
restoring pages. Conflicts return busy errors; no durable lock sidecar is
created. These locks protect cooperating MyLite processes from unsafe concurrent
access but are not the final multi-writer lock manager.

This is not SQL transaction support yet. The MyLite handler still advertises
non-transactional engine flags, and MariaDB commit, rollback, and savepoint
handlerton hooks remain planned.

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
