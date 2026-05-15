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
serve ordered handler cursors from MariaDB key tuples, including bounded
BLOB/TEXT prefix key images produced by MariaDB. `TRUNCATE TABLE` logically
deletes live rows and resets autoincrement state without changing catalog
metadata. `DROP TABLE` removes catalog metadata for routed tables.
Simple `RENAME TABLE` updates catalog identity while preserving table ids, row
pages, and index-entry pages. Copy `ALTER` rebuilds use MariaDB's table-copy
path and append rebuilt table definitions, rows, and supported index entries
inside the primary file. `CREATE TABLE ... LIKE` uses MariaDB's clone-definition
path and publishes an empty MyLite catalog record with source requested-engine
metadata preserved. Online `ALTER`, in-place `ALTER`, transaction-aware index
maintenance, free-space reclamation, and unsupported index classes still reject
or remain planned until those slices define the paths. Standalone
`CREATE INDEX` and `DROP INDEX` use MariaDB's ALTER-backed DDL path for
supported copy-rebuild index additions and drops. File-backed opens answer
MariaDB SQL-layer schema and table discovery from MyLite catalog namespace
records when no transient runtime schema directory exists.
The SQL layer forces routed MyLite `ALTER TABLE` statements onto the copy
algorithm before MariaDB's in-place ALTER preparation can write temporary
`.frm` files under schema directories; MyLite does not support in-place ALTER
yet. Storage-smoke coverage includes representative default-algorithm column,
index, standalone-index, CHECK, and autoincrement ALTER operations after
catalog-only reopen without a rehydrated runtime schema directory.
Foreign-key DDL is rejected at the `libmylite` boundary until MyLite has
catalog metadata, enforcement, locking, recovery, and transaction-aware checks
for referential constraints.
Partition DDL is rejected at the same boundary until MyLite has partition
metadata, partition-to-primary-file routing, per-partition catalog lifecycle,
and partition-aware row and index maintenance.
Basic CHECK constraints are kept inside the MariaDB table-definition image and
evaluated by MariaDB before MyLite handler writes. Supported copy
`ALTER TABLE` paths can add and drop named table-level CHECK constraints
through the same definition bridge; MyLite does not implement a separate
constraint-expression evaluator.
Basic generated columns are also routed through MariaDB's virtual column
machinery. MyLite advertises generated-column support to MariaDB, stores
persistent generated values in normal row payloads, restores base row buffers so
MariaDB can compute non-stored virtual values after reads, supports copy ALTER
add/modify/drop for generated columns, and stores ordinary generated-column
secondary/unique key tuples in MyLite index-entry pages.

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
pages. Schema namespace names use lightweight catalog records; table-definition
schema names are also treated as namespaces for compatibility with files that
pre-date explicit schema records. Row inserts append checksummed row pages
tagged by catalog table id;
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
path and publish rebuilt rows with matching index-entry pages. Representative
default-algorithm copy ALTER paths after catalog-only reopen cover column
add/drop/rename, ALTER-backed index add/drop, standalone index create/drop, and
autoincrement metadata updates. `LOCK=NONE` copy ALTER, in-place ALTER,
unsupported index rebuilds, and transactional DDL rollback remain planned until
MyLite has locking and recovery.
`CREATE TABLE ... LIKE` clones supported routed source table definitions through
MariaDB's normal LIKE path, does not copy rows, resets target autoincrement
state, and records the source requested engine with effective `MYLITE` when the
statement has no explicit engine.
Successful supported `CREATE TABLE ... SELECT` uses MariaDB's `select_create`
path to derive or open the target definition and then inserts result rows
through MyLite's normal `write_row()` path, including projections that read
virtual and stored generated columns from MyLite source tables into ordinary
target columns. Duplicate-key CTAS abort follows MariaDB's target-drop path and
removes target catalog metadata; the current statement checkpoint restores
pre-statement MyLite header/catalog visibility for covered failed file-backed
statements.
Representative user temporary `CREATE TABLE ... LIKE` and
`CREATE TABLE ... SELECT` paths use MariaDB's temporary-table lifecycle while
keeping SQL-visible temporary names out of the durable user schema catalog.
Storage-smoke coverage verifies the temporary tables are usable during the
session, are cleaned up after `DROP TEMPORARY TABLE`, and are gone after
close/reopen.
Successful representative `CREATE OR REPLACE TABLE ... LIKE` and
`CREATE OR REPLACE TABLE ... SELECT` statements use MariaDB's drop-then-create
flow: MyLite removes the old catalog record, publishes the replacement
definition, writes replacement rows and indexes where applicable, and verifies
close/reopen visibility. Representative failed OR REPLACE rollback covers
self-LIKE rejection, unsupported replacement definitions, and duplicate-key
replacement CTAS while preserving old target metadata, rows, indexes, and
autoincrement state through the existing statement checkpoint; broader
temporary, locking, and SQL transaction/savepoint semantics remain planned.
Basic column-level and named table-level CHECK constraints survive close/reopen
because they are stored in the catalog-backed table-definition image. MariaDB
enforces those checks before insert/update handler calls unless
`check_constraint_checks=OFF` is set. Supported copy `ALTER TABLE` paths cover
named table-level CHECK additions and drops, including dropping an ALTER-added
CHECK after catalog-only close/reopen. Supported CTAS paths cover explicit
CHECK-constrained target definitions and CHECK-violation target cleanup.
Failed ADD CHECK copy ALTER over incompatible existing rows restores visible
pre-statement catalog and row state through the existing statement checkpoint.
Prepared execution diagnostics are covered for representative CHECK failures.
Representative dump-style fixture import is covered for CHECK definitions.
Representative `SHOW CREATE TABLE` round-trip export/import is covered for
CHECK definitions.
Representative deterministic CHECK expression matrices are covered. Exhaustive
CHECK expression, broader failed ALTER rollback, broader dump/export, and
transaction rollback coverage remains planned.
Basic virtual and stored generated columns follow the same catalog-backed
table-definition path, including supported copy ALTER add/modify/drop
operations, CTAS projections from generated source columns, and generated
target CTAS definitions. Ordinary secondary and unique indexes on scalar
virtual or stored generated columns use the same MariaDB-generated key tuples
as supported base-column indexes, including initial definitions and supported
copy-rebuild add, drop, rename, and standalone index DDL paths. Bounded
generated BLOB/TEXT prefix indexes declared in initial table definitions or
added through standalone copy-rebuild index DDL use the same generated-value and
BLOB/TEXT prefix key-image paths. Generated
primary-key DDL inherits MariaDB's SQL-layer rejection before catalog
publication. Unbounded unique BLOB/TEXT keys that MariaDB represents as hidden
long-unique hash metadata reject before catalog publication, including generated
BLOB/TEXT columns. MariaDB 11.8 does not expose MySQL-style base-table
expression key-part syntax; full BLOB/TEXT index support, MySQL-style
expression-index compatibility, and exhaustive expression matrices remain planned.
Prepared execution diagnostics are covered for representative generated-column
unique-key failures.
Representative dump-style fixture import is covered for generated-column
definitions and generated-column indexes.
Representative `SHOW CREATE TABLE` round-trip export/import is covered for
generated-column definitions and indexes.
Representative deterministic generated-column expression matrices are covered.
The same create-time key-shape gate rejects FULLTEXT, SPATIAL, and long-unique
hash indexes before catalog publication; MyLite must not publish a table
definition whose index class cannot be maintained by the current storage
format.

## Schemas And System Surfaces

MariaDB's `database.table` model maps to catalog namespaces:

```text
schema_id -> schema name
table_id  -> schema_id + table name
index_id  -> table_id + index name
```

No persistent directory is created for a schema. `CREATE DATABASE`,
`DROP DATABASE`, `ALTER DATABASE`, `USE`, table-name resolution, and
information schema listing are represented as catalog namespaces. Schema
records store the schema name plus the default character set, default
collation, and schema comment that MariaDB would otherwise keep in `db.opt`.
The current implementation keeps newly-created schema directories as transient
runtime state when MariaDB's active table-DDL paths still need them, but
file-backed reopen no longer reconstructs those directories from the catalog.
SQL-layer hooks answer schema existence, schema/table listing, option reads,
and no-directory schema alter/drop paths from catalog records. Final
filesystem-free hooks for all schema DDL and non-table database objects remain
planned.

Views, triggers, routines, packages, sequences, and routine invocation are
rejected by the current `libmylite` SQL policy before MariaDB can publish
filesystem-backed or server-table-backed metadata. Catalog-backed persistence
for those object classes requires a separate format and execution design.
Foreign-key DDL is also rejected by policy so routed `ENGINE=InnoDB` metadata
does not imply referential-integrity enforcement before MyLite storage supports
it.

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
map. `truncate()` appends delete row-state pages for all live row ids and
resets table-local autoincrement state to the first generated value. Explicit
`ALTER TABLE ... AUTO_INCREMENT` publishes an exact table-local next value
before and after catalog-only reopen, and copy `ALTER` row movement can advance
that value above copied live row data. Routed autoincrement definitions are
accepted only when the autoincrement column has a complete single-column key;
compound-only autoincrement key definitions reject before catalog publication
until allocation semantics are designed.
Row, overflow, index-entry, and old autoincrement pages remain orphaned until
compaction exists. Nullable fixed and variable fields are covered because the
stored record image includes MariaDB's null bitmap. BLOB/TEXT fields are
serialized as length-prefixed value bytes, not process pointers, and large
payloads use primary-file overflow pages.

Supported primary, unique, and secondary keys use MariaDB key tuples generated
from the row buffer. Bounded BLOB/TEXT prefix indexes are supported by storing
MariaDB's normal variable-length key image, not row-buffer process pointers.
The handler rejects unsupported key classes before table publication, including
FULLTEXT, SPATIAL, hidden generated, hash, long-unique hash, and oversized or
unbounded BLOB/TEXT keys.
Duplicate checks read live index entries, use MariaDB key comparison, and
preserve nullable unique-key semantics. Ordered index reads build in-memory
cursors from live index entries and then reconstruct row buffers from row
pages. This provides correct indexed insert, lookup, update, delete, reopen,
and copy `ALTER` behavior for the supported shapes, but it is not the final
performance structure. Standalone `CREATE INDEX` and `DROP INDEX` are covered
for supported copy-rebuild index definitions. B-tree pages, free-space
reclamation, multi-statement transaction rollback, and transaction-aware index
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

FULLTEXT, SPATIAL, MySQL-style expression indexes, foreign-key enforcement, and
partitioned tables need explicit storage designs before support is claimed.
Generated primary keys follow MariaDB's SQL-layer rejection policy. Long-unique
hash keys remain unsupported until MyLite has a durable hidden-key design.
Current `libmylite` entry points reject foreign-key and partition DDL before
MariaDB execution; unsupported FULLTEXT, SPATIAL, and long-unique indexes reject
through handler capability checks before catalog publication.

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

Storage operations now use advisory locks on the primary `.mylite` file
descriptor. Read APIs take a shared lock after pending recovery is handled,
while writes and recovery take an exclusive lock before mutating or restoring
pages. Conflicts return busy errors after the current thread's configured busy
timeout expires; a zero timeout keeps immediate non-blocking behavior. No
durable lock sidecar is created. These locks protect cooperating MyLite
processes from unsafe concurrent access but are not the final multi-writer lock
manager.

File-backed MyLite statements that can mutate storage now take an in-process
statement checkpoint. Row DML begins that checkpoint from the MyLite handler
when MariaDB write-locks a routed table, registers the handlerton in MariaDB's
statement transaction list, and releases or restores the checkpoint from
MariaDB's statement commit/rollback hooks. DDL and catalog paths that do not
reliably enter `external_lock()` keep the outer `libmylite` checkpoint before
MariaDB execution.

The checkpoint saves the committed header and catalog root pages while holding
the primary-file exclusive lock; storage APIs in the same thread borrow that
locked descriptor. If MariaDB reports statement failure, MyLite restores the
saved catalog/header pages so rows, row-state pages, index entries,
autoincrement pages, and catalog records appended after the checkpoint are no
longer visible. Successful statements release the checkpoint after handler
statement commit or required schema-catalog synchronization.

This is still not SQL transaction support. The MyLite handler still advertises
non-transactional engine flags. Public `libmylite` SQL entry points reject
explicit transaction-control and autocommit-control statements before MariaDB
execution so routed `ENGINE=InnoDB` tables do not imply multi-statement
rollback semantics. Full transaction, savepoint, and transactional engine-flag
support remains planned.

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
