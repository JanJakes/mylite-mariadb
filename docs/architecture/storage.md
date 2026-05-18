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
BLOB/TEXT prefix key images produced by MariaDB. Those cursors keep sorted key
metadata and row ids, then materialize the selected row payload lazily when the
SQL layer asks for a row. `TRUNCATE TABLE` logically
deletes live rows and resets autoincrement state without changing catalog
metadata. Ordinary `CREATE TABLE IF NOT EXISTS` creates missing routed tables
and skips existing routed tables without changing their catalog metadata.
`DROP TABLE` removes catalog metadata for routed tables.
Simple `RENAME TABLE` updates catalog identity while preserving table ids, row
pages, and index-entry pages. Representative failed multi-table DROP/RENAME
paths restore the statement-start catalog and row/index visibility through the
statement checkpoint. Representative table-DDL `IF EXISTS` statements skip
missing DROP/RENAME targets through MariaDB's warning semantics while applying
existing routed-table catalog mutations in the same statement. Copy `ALTER`
rebuilds use MariaDB's table-copy path and append rebuilt table definitions,
rows, and supported index entries inside the primary file. `CREATE TABLE ...
LIKE` uses MariaDB's clone-definition path and publishes an empty MyLite catalog
record with source requested-engine metadata preserved. FK child source tables
clone their ordinary table and index shape, but MyLite does not copy source FK
metadata to the LIKE target. Online `ALTER`, in-place `ALTER`,
transaction-aware index maintenance, free-space reclamation, and unsupported
index classes still reject
or remain planned until those slices define the paths. Standalone
`CREATE INDEX` and `DROP INDEX` use MariaDB's ALTER-backed DDL path for
supported copy-rebuild index additions and drops, including representative
`CREATE INDEX IF NOT EXISTS` and `DROP INDEX IF EXISTS` skips. File-backed
opens answer MariaDB SQL-layer schema and table discovery from MyLite catalog
namespace records when no transient runtime schema directory exists.
The SQL layer forces routed MyLite `ALTER TABLE` statements onto the copy
algorithm before MariaDB's in-place ALTER preparation can write temporary
`.frm` files under schema directories; MyLite does not support in-place ALTER
yet. Storage-smoke coverage includes representative default-algorithm column,
index, standalone-index, CHECK, and autoincrement ALTER operations after
catalog-only reopen without a rehydrated runtime schema directory.
Representative online and in-place ALTER requests, including `LOCK=NONE`,
`ALTER ONLINE TABLE`, and `ALGORITHM=INPLACE` / `INSTANT` / `NOCOPY`, are
rejected by the MyLite SQL policy before MariaDB execution.
The first public foreign-key DDL subset publishes catalog-backed FK metadata
for supported `CREATE TABLE` and copy `ALTER TABLE ... ADD FOREIGN KEY`
statements. The subset requires durable MyLite-routed base tables, explicit or
MariaDB-generated supported child key prefixes, exact unique parent keys, and
immediate `RESTRICT` / `NO ACTION` semantics. Basic self-referential
constraints are covered when child rows reference parent rows that already
exist, and same-row self-references are covered when the child and referenced
parent key prefixes match in the same new row. The storage layer stores typed
FK blob pages, supports child and parent FK listing, exposes handler metadata
and `SHOW CREATE TABLE` hooks, advertises MyLite's covered FK subset through
the MariaDB handlerton, preserves retained metadata across MariaDB's internal
old-table backup rename, and performs FK-aware column/supporting-key checks
with handler-owned retained supporting-key validation plus immediate
child/parent row checks. Supported copy
`ALTER TABLE ... DROP FOREIGN KEY` removes FK metadata from the primary file
and disables the dropped constraint's row checks across close/reopen. SQL-level
`DROP TABLE` rejects referenced-parent drops while supported FK metadata remains
and removes child-table FK metadata with dropped child tables so the parent can
be dropped afterward. Session
`foreign_key_checks=0` disables supported FK row checks and parent-table
truncate checks without retrospective validation when checks are re-enabled.
Fixture-backed dump coverage imports child rows before parent rows under that
session bypass, then proves later violating writes fail after checks are
restored.
MariaDB-generated supporting keys are cleaned up when copy ALTER replaces them
with explicit compatible keys, and can be explicitly dropped after the owning
FK is removed. Referenced parent unique secondary-key renames update the FK
referenced-key metadata and preserve parent row checks across close/reopen for
the supported `RESTRICT` / `NO ACTION` subset. Non-self child and parent row
checks resolve the actual referenced or child supporting index before probing
stored index entries, so unrelated indexes with matching key prefixes do not
affect FK enforcement. Ordered multi-row child and self-referential inserts are
covered for the supported FK subset, including failed-statement rollback when a
later row violates the constraint.
Representative ordered self-referential update/delete checks cover
parent-first rejection and child-first success. Representative non-self parent
update/delete ordering checks cover failed-statement rollback when an earlier
unreferenced parent row was processed before a later referenced parent row.
Representative multi-table update/delete ordering checks cover parent-first
rejection and child-first success when the target-table order is forced.
Representative parent-target multi-table action checks cover `ON DELETE
CASCADE`, `ON DELETE SET NULL`, `ON UPDATE CASCADE`, and `ON UPDATE SET NULL`
dispatch from joined statements while child rows are mutated only by the
foreign-key action.
Bounded self-referencing, same-row self-referencing, and non-self
`ON DELETE SET NULL` / `ON UPDATE SET NULL` actions, bounded
`ON DELETE CASCADE`, bounded recursive `ON UPDATE CASCADE` over acyclic action
chains, and supported combinations of those actions, including the bounded
same-row update action matrix for `ON UPDATE SET NULL` and
`ON UPDATE CASCADE`, are supported for
simple durable tables with nullable child FK columns where required and no
BLOB/TEXT or generated columns; the handler mutates matching child rows,
deletes matching cascade children, or rewrites the current same-row update
buffer before deleting or updating the parent row.
`SET DEFAULT` action clauses are rejected explicitly because the selected
MariaDB/InnoDB base documents the action as unsupported. Broader exhaustive
multi-table matrices, deferrable set-wide validation, and cyclic or full
recursive action graphs remain planned.
Partition DDL and representative partition-management statements remain
rejected at the `libmylite` boundary until MyLite has partition metadata,
partition-to-primary-file routing, per-partition catalog lifecycle, and
partition-aware row and index maintenance.
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
pre-date explicit schema records. Internal FK definitions use catalog records
keyed by child schema/table and constraint name plus typed FK blob pages for
referenced key, column-list, action, match-option, and nullable-column
metadata. FK referenced-key rewrites append replacement FK blob pages and
republish the FK catalog record. Row inserts append checksummed row pages
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
rollback journal before the header or catalog root page is overwritten. Active
file-backed row-DML transactions create a transient transaction journal
with the transaction-start header and catalog root pages; recovery applies any
ordinary rollback journal first, then the transaction journal. Richer
transaction state, free-space metadata, and B-tree-style index navigation are
still planned slices.

The catalog stores:

- schemas,
- table definitions,
- table-definition binary images needed by MariaDB discovery,
- columns, indexes, constraints, and engine metadata,
- validated foreign-key metadata for the supported public FK SQL subset,
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
`ENGINE=MyISAM`, `ENGINE=Aria`, `ENGINE=BLACKHOLE`, `ENGINE=MEMORY`, and
`ENGINE=HEAP` requests. The catalog records both the requested engine name and
the effective `MYLITE` engine. BLACKHOLE-routed tables persist only table
metadata and discard row writes instead of publishing MyLite row or index-entry
pages. MEMORY/HEAP-routed tables persist only table metadata in the primary
file; row contents and supported index entries live in a process-runtime
volatile store and are cleared when the embedded MariaDB runtime shuts down.
Those MEMORY/HEAP volatile rows participate in the current bounded MyLite
statement, transaction, and savepoint rollback model through in-memory
snapshots, while preserving generated autoincrement gaps. User temporary tables
also use the volatile row store, but they are explicitly excluded from these
snapshots so their rows and create/drop lifecycle follow MariaDB's ordinary
temporary-table transaction behavior.
Unsupported explicit `ENGINE` table options, including known external
no-equals engine names, fail before catalog publication.
Ordinary
`CREATE TABLE IF NOT EXISTS` statements publish missing targets through the
normal create path and
leave existing routed targets unchanged while exposing MariaDB warnings through
the public warning API. `DROP TABLE` removes the live catalog record and
increments the catalog generation without deleting external MariaDB sidecars.
Referenced-parent drops fail while supported FK metadata still points at the
table. Child-table drops remove child FK metadata and the table record in one
catalog publication, after which the parent can be dropped normally. Dropped
table-definition blobs, row pages, index-entry pages, and FK metadata blob pages
remain orphaned inside the primary file until free-space management exists; new
table ids are allocated above both live catalog records and existing row pages
so drop/recreate does not expose old rows. Simple
`RENAME TABLE` rewrites the catalog record identity while preserving table id,
requested/effective engine metadata, and the stored table-definition blob
reference, so existing row and index-entry pages move with the renamed table.
Representative failed multi-table `DROP TABLE` and `RENAME TABLE` statements
preserve original table metadata, rows, and supported index reads before and
after close/reopen through the statement checkpoint. `DROP TABLE IF EXISTS`
and `RENAME TABLE IF EXISTS` inherit MariaDB's missing-object skip semantics
for routed base tables; MyLite applies existing-table catalog mutations, leaves
skipped missing names absent, and exposes the MariaDB warnings through the
public warning API.
Copy `ALTER` rebuilds let MariaDB create a temporary MyLite table, copy rows
through `ha_write_row()`, rename the old table to a backup, rename the rebuilt
table to the final name, and drop the backup catalog record. This preserves
requested engine metadata for implicit rebuilds and records explicit supported
engine requests on engine rebuilds.
Supported key additions on copy `ALTER` rebuild through the same table-copy
path and publish rebuilt rows with matching index-entry pages. Representative
default-algorithm copy ALTER paths after catalog-only reopen cover column
add/drop/rename including representative `ADD COLUMN IF NOT EXISTS` and
`DROP COLUMN IF EXISTS` skips plus representative `MODIFY COLUMN IF EXISTS` and
`CHANGE COLUMN IF EXISTS` skips, `RENAME COLUMN IF EXISTS` skips, and
`ALTER COLUMN IF EXISTS SET/DROP DEFAULT` skips, ALTER-backed index add/drop,
standalone index create/drop including representative existence-option skips,
primary-key add/drop/re-add including duplicate `ADD PRIMARY KEY IF NOT EXISTS`
warnings and failed re-add rollback over duplicate rows, failed unique-key add
rollback over duplicate existing rows, duplicate and missing
`ADD CONSTRAINT ... UNIQUE IF NOT EXISTS` paths, existing and missing unique-key
`DROP CONSTRAINT IF EXISTS` paths, explicit unique-constraint key-name
semantics, named composite unique constraints through copy ALTER, and
autoincrement metadata updates. Failed strict copy ALTER conversion from an
existing column to an incompatible target column type restores the old catalog
and row/index visibility through the existing statement checkpoint.
`LOCK=NONE` and in-place/instant/no-copy ALTER requests are explicitly
rejected until MyLite has online DDL and lock integration.
Unsupported index rebuilds and transactional DDL rollback remain planned until
MyLite has locking and recovery.
`CREATE TABLE ... LIKE` clones supported routed source table definitions through
MariaDB's normal LIKE path, does not copy rows, resets target autoincrement
state, and records the source requested engine with effective `MYLITE` when the
statement has no explicit engine. When the source table is a supported FK child,
the cloned target keeps the ordinary columns and supporting indexes, but source
FK metadata is not copied into MyLite's FK catalog.
Successful supported `CREATE TABLE ... SELECT` uses MariaDB's `select_create`
path to derive or open the target definition and then inserts result rows
through MyLite's normal `write_row()` path, including projections that read
virtual and stored generated columns from MyLite source tables into ordinary
target columns. Duplicate-key CTAS abort follows MariaDB's target-drop path and
removes target catalog metadata; the current statement checkpoint restores
pre-statement MyLite header/catalog visibility for covered failed file-backed
statements.
Explicit FK-constrained CTAS targets are covered for the current supported
`RESTRICT` / `NO ACTION` subset: the target FK metadata is published with the
table definition, selected rows run through child-row checks, failed FK row
checks remove the target catalog record, and the FK remains visible and
enforced after close/reopen.
Representative `CREATE TABLE ... IGNORE SELECT` and
`CREATE TABLE ... REPLACE SELECT` coverage follows MariaDB's CTAS duplicate
mode handling over supported MyLite primary, unique, and secondary indexes with
deterministic ordered SELECT input.
Representative user temporary `CREATE TABLE ... LIKE` and
`CREATE TABLE ... SELECT` paths use MariaDB's temporary-table lifecycle while
keeping SQL-visible temporary names out of the durable user schema catalog.
Storage-smoke coverage verifies the temporary tables are usable during the
session, same-name temporary tables shadow durable base tables until
`DROP TEMPORARY TABLE`, durable tables become visible again after the temporary
drop, and temporary tables are gone after close/reopen.
Representative `CREATE OR REPLACE TEMPORARY TABLE` coverage verifies LIKE
replacement over a same-name durable shadow, CTAS replacement from a distinct
durable source, and new same-name temporary CTAS over a durable source. A
repeated same-name temporary CTAS replacement that also reads the same SQL name
hits MariaDB's temporary-table reopen guard and remains a tracked compatibility
edge case rather than a MyLite storage claim.
Successful representative plain `CREATE OR REPLACE TABLE`,
`CREATE OR REPLACE TABLE ... LIKE`, and
`CREATE OR REPLACE TABLE ... SELECT` statements use MariaDB's
drop-then-create flow: MyLite removes the old catalog record, publishes the
replacement definition, writes replacement rows and indexes where applicable,
and verifies close/reopen visibility. The plain replacement coverage verifies
old rows, old indexes, and old autoincrement state are not SQL-visible after
replacement. Representative plain replacement coverage for generated columns
and CHECK constraints verifies old metadata is not SQL-visible, while
replacement generated-column indexes and CHECK constraints survive close/reopen.
Representative failed OR REPLACE rollback covers self-LIKE
rejection, missing-source LIKE/CTAS inputs, unsupported replacement
definitions, and duplicate-key replacement CTAS while preserving old target
metadata, rows, indexes, and autoincrement state through the existing statement
checkpoint. FK-aware OR REPLACE coverage
rejects plain, LIKE, and CTAS replacement of a referenced parent without
dropping parent rows, child rows, or FK metadata, and verifies LIKE and CTAS
replacement of child tables removes old FK metadata before publishing the
replacement definition. Representative failed
multi-table DROP/RENAME rollback preserves original target metadata, rows, and
indexes through the same checkpoint, including child FK metadata for covered
FK table shapes; broader locking, temporary-table edge cases, and SQL
transaction/savepoint semantics remain planned.
Representative `SHOW CREATE TABLE` round-trip coverage includes both a
generated/CHECK/indexed table shape and an FK parent/child pair exported after
catalog-backed reopen and imported into a fresh schema with FK checks and
supported actions preserved. A representative ALTER-evolved table is also
exported after generated-column, CHECK, unique-key, and prefix-index copy
ALTERs and imported into a fresh schema.
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
CHECK definitions, including a representative table whose CHECK constraints
were added through copy ALTER before export.
Representative deterministic CHECK expression matrices cover string,
NULL-handling, conditional, temporal, and numeric expressions. Exhaustive CHECK
expression, broader failed ALTER rollback beyond the covered ADD CHECK,
ADD UNIQUE, generated-dependency, and strict conversion shapes, broader
dump/export, and transaction rollback coverage remains planned.
Basic virtual and stored generated columns follow the same catalog-backed
table-definition path, including supported copy ALTER add/modify/drop
operations, CTAS projections from generated source columns, and generated
target CTAS definitions. Ordinary secondary and unique indexes on scalar
virtual or stored generated columns use the same MariaDB-generated key tuples
as supported base-column indexes, including initial definitions and supported
copy-rebuild add, drop, rename, standalone index DDL, generated-column
`ADD CONSTRAINT ... UNIQUE` / `DROP CONSTRAINT` paths, and failed generated
unique constraint adds over duplicate generated values. Representative
composite unique constraints over virtual generated columns use the same key
tuple and retained metadata paths. Bounded
generated BLOB/TEXT prefix indexes declared in initial table definitions or
added through standalone copy-rebuild index DDL use the same generated-value and
BLOB/TEXT prefix key-image paths. Generated
primary-key DDL inherits MariaDB's SQL-layer rejection before catalog
publication. Unbounded unique BLOB/TEXT keys that MariaDB represents as hidden
long-unique hash metadata reject before catalog publication, including generated
BLOB/TEXT columns. MariaDB 11.8 does not expose MySQL-style base-table
expression key-part syntax; full BLOB/TEXT index support, MySQL-style
expression-index compatibility, and exhaustive expression matrices remain planned.
Failed dependent column drops for generated-column base columns leave the
original generated metadata, generated index entries, and rows visible.
Representative failed multi-row direct insert and prepared update statements
restore stored and virtual generated base values and generated-index visibility
through statement rollback checkpoints.
Representative strict-mode generated expression failures also restore row and
generated-index visibility after earlier attempted writes in the failed
statement, including representative grouped ODKU duplicate-update expression
failures.
Nullable composite unique constraints preserve MariaDB NULL semantics through
the same retained-key and key-tuple paths: exact non-NULL duplicates reject, but
rows with NULL in nullable key parts do not conflict.
Prepared execution diagnostics are covered for representative generated-column
unique-key failures.
Representative dump-style fixture import is covered for generated-column
definitions and generated-column indexes.
Representative `SHOW CREATE TABLE` round-trip export/import is covered for
generated-column definitions and indexes, including a representative table
whose generated columns and generated indexes were added through copy ALTER
before export.
Representative deterministic generated-column expression matrices cover string,
NULL-handling, conditional, temporal, and numeric expressions.
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
SQL-layer hooks answer file-backed initial schema creation, schema existence,
schema/table listing, option reads, catalog-backed `CREATE DATABASE` existence
options, and no-directory schema alter/drop paths from catalog records.
Non-table database objects remain planned.

Views, triggers, routines, packages, sequences, and routine invocation are
rejected by the current `libmylite` SQL policy before MariaDB can publish
filesystem-backed or server-table-backed metadata. Catalog-backed persistence
for those object classes requires a separate format and execution design.
Foreign-key DDL is also rejected by policy so routed `ENGINE=InnoDB` metadata
does not imply referential-integrity enforcement before MyLite storage supports
it. Handler hooks expose only MyLite-owned internal FK metadata that was seeded
through first-party storage primitives.

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
accepted when the autoincrement column is the first part of a supported key,
including single-column and first-key compound keys. Grouped later-in-key
autoincrement definitions are also accepted for routed tables; MyLite advertises
MariaDB's auto-part-key handler capability and allocates generated values by
comparing live index entries for the current key prefix and fetching the
maximum matching row, including stale delete/update filtering and reverse-sort
autoincrement definitions. Representative
`auto_increment_offset` / `auto_increment_increment` coverage includes
single-row and multi-row post-explicit allocation for both first-key and grouped
prefix table shapes plus a broader pair matrix including offset greater than
increment. Representative integer-width overflow coverage verifies the last
valid generated value and next generated overflow for signed and unsigned
`TINYINT`, signed `SMALLINT`, signed and unsigned `MEDIUMINT`, signed and
unsigned `INT`, and signed `BIGINT`, plus unsigned `SMALLINT` with non-default
offset/increment settings.
Runtime-volatile MEMORY/HEAP autoincrement overflow uses the same SQL-layer
boundary behavior while keeping rows and autoincrement state out of durable
MyLite row pages.
Explicit `BIGINT UNSIGNED` maximum values are allowed for first-key,
grouped-prefix, and MEMORY/HEAP tables; the following generated value fails
through MariaDB's `ULONGLONG_MAX` autoincrement read-failed sentinel.
For first-key table-local autoincrement state, completed durable transaction
rollback and nested direct savepoint rollback remove generated rows while
republishing advancing next values for tables that existed at the checkpoint,
matching MariaDB/InnoDB's persistent-but-non-transactional gap behavior for
`ROLLBACK` and `ROLLBACK TO SAVEPOINT`. Durable failed and ignored generated
insert statements also preserve generated gaps by marking only row-DML
checkpoints for autoincrement preservation. Durable first-key generated
statements publish MariaDB-requested reservation intervals from
`get_auto_increment()` before row publication, so failed multi-row inserts keep
the reserved gap even when later generated rows never reach `write_row()`.
Source-driven `INSERT ... SELECT` statements use MariaDB's unknown-row-count
reservation growth, so failed statements roll back visible rows while preserving
the reserved generated boundary, and `INSERT IGNORE ... SELECT` skips duplicate
source rows while preserving the reserved tail for the next statement.
Mixed generated `INSERT IGNORE` statements may assign consecutive ids to
successful rows around a skipped duplicate row, but the next statement still
resumes after the reserved interval boundary.
`INSERT ... ON DUPLICATE KEY UPDATE` follows the same durable first-key
reservation path before duplicate-key checks. A duplicate-update row can reuse
the statement-local generated cursor for later successful rows, while the next
statement still resumes after the reserved interval boundary.
`INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` uses MariaDB's unknown
source-row-count reservation growth, so durable next values resume after the
latest reserved interval boundary even when selected duplicate rows update
existing rows instead of inserting new ones.
Grouped later-in-key `INSERT ... ON DUPLICATE KEY UPDATE`, including
source-driven and prepared representative forms, keeps the per-prefix
allocation model: generated values are derived from the current live prefix
maximum, so duplicate-update attempts do not create a first-key-style
table-local reserved tail gap, while explicit high-value duplicate updates
advance only their own prefix.
When a grouped duplicate-update branch fails after earlier row publication in
the statement, including representative source-read, update-expression, and
generated-expression, and CHECK-constraint errors, rollback removes the
published rows and the next statement still recomputes from the live prefix
maximum.
SQL-layer failures before generated value allocation, such as CHECK failures on
the initial insert candidate before handler writes, do not reserve MyLite
values.
Failed DDL checkpoints do not inherit the row-DML preservation marker.
Explicit high-value updates to first-key autoincrement columns advance the
table-local next value only after the MyLite update path passes duplicate-key
and foreign-key checks. Failed duplicate-key updates and duplicate-key
`UPDATE IGNORE` skips therefore leave the attempted high value unused, while a
successful high-value update advances the next generated value and close/reopen
state.
Grouped later-in-key `UPDATE IGNORE` skips follow the live-prefix rule: an
attempted explicit high grouped id is ignored with the skipped row, and the
next generated value is derived from the live per-prefix maximum.
If a multi-row update publishes a successful explicit high-value advancement
before a later row fails, statement rollback restores row/index visibility while
preserving the published autoincrement advancement for the next generated row;
representative coverage includes later foreign-key, duplicate-key, and CHECK
failures.
Failed multi-row updates that hit MyLite foreign-key checks before update-row
publication likewise leave attempted explicit high values unused.
Duplicate-update branches that explicitly set the autoincrement column advance
the durable next value through the ordinary MyLite update path, including
transaction rollback preservation and close/reopen persistence.
Representative direct and prepared routed ODKU statement effects expose
MariaDB-compatible affected-row counts and insert ids through the public
`libmylite` APIs, including duplicate updates, multi-row `INSERT ... VALUES`
and `INSERT ... SELECT` generated inserts, and explicit `LAST_INSERT_ID(id)`
duplicate-update branches.
Failed duplicate-update branches after earlier generated row publication roll
back visible row/index changes while preserving the generated reservation
boundary for both `INSERT ... VALUES` and `INSERT ... SELECT` ODKU statements.
Explicit high-value inserts likewise advance only after MyLite duplicate-key
and FK checks pass, so failed explicit duplicate inserts and ignored skips do
not consume the attempted value.
The grouped autoincrement path is correct for the supported storage subset but
still scans append-only index entries until storage-level B-tree navigation
exists.
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
preserve nullable unique-key semantics. Durable index-entry scans use the
index-entry table id and row-state tombstone map for visibility, then defer
row-page validation until a selected row is materialized. Ordered index reads
build in-memory cursors from live index entries, keep sorted key metadata plus
row ids, use bound searches for key positioning, build filtered cursors for
exact-key and prefix reads, stop after the first matching non-nullable full-key
unique integer entry, and use storage-level exact-entry or exact-entryset lookup
for guarded raw equality paths so the handler does not allocate unrelated index
entries for common integer point reads. Cursors check `index_next_same()`
boundaries before row materialization and reconstruct only the selected row
buffer from row pages.
This provides correct indexed insert, lookup, update, delete, reopen, and copy
`ALTER` behavior for the supported shapes, but it is still an interim
performance structure because exact lookup still scans live append-only entry
pages. Standalone
`CREATE INDEX` and `DROP INDEX` are covered for supported copy-rebuild index
definitions. B-tree pages, free-space reclamation, multi-statement transaction
rollback, and transaction-aware index maintenance remain planned.

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

FULLTEXT, SPATIAL, MySQL-style expression indexes, broader foreign-key
semantics, and partitioned tables need explicit storage designs before support
is claimed.
Generated primary keys follow MariaDB's SQL-layer rejection policy. Long-unique
hash keys remain unsupported until MyLite has a durable hidden-key design.
Current `libmylite` entry points still reject `CREATE TEMPORARY TABLE` FK DDL
and partition DDL or management statements before MariaDB execution.
Unsupported FK shapes, FULLTEXT, SPATIAL, and long-unique indexes reject
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
previously committed header/catalog state and truncates the primary file to the
restored header page count. Committed orphan pages from update/delete/truncate
history still wait for free-space reclamation.

Storage operations now use advisory locks on the primary `.mylite` file
descriptor. Read APIs take a shared lock after pending recovery is handled,
while writes and recovery take an exclusive lock before mutating or restoring
pages. Conflicts return busy errors after the current thread's configured busy
timeout expires; a zero timeout keeps immediate non-blocking behavior. No
durable lock sidecar is created. These locks protect cooperating MyLite
processes from unsafe concurrent access but are not the final multi-writer lock
manager. If a cooperating writer holds an active row-DML transaction lock and
has published a valid transaction journal, cross-process storage reads use the
journal's transaction-start header and catalog root pages as a read-only
snapshot; stale journals without an active exclusive writer still require the
exclusive recovery path.

File-backed MyLite statements that can mutate storage now take in-process
statement checkpoints. Autocommit row DML begins that checkpoint from the
MyLite handler when MariaDB write-locks a routed table, registers the
handlerton in MariaDB's statement transaction list, and releases or restores
the checkpoint from MariaDB's statement commit/rollback hooks. DDL and catalog
paths that do not reliably enter `external_lock()` keep the outer `libmylite`
checkpoint before MariaDB execution.
The handler statement context also owns a volatile snapshot for routed
MEMORY/HEAP rows, so failed statements restore process-local row and supported
index visibility at the same MariaDB statement boundary. User temporary
volatile tables stay excluded from these snapshots.

Direct or prepared `libmylite` `BEGIN` / `START TRANSACTION` / `COMMIT` /
`ROLLBACK` and supported direct or prepared session
`SET autocommit=0/1/DEFAULT` support, including `SET` lists with ordinary
non-transaction assignments and duplicate supported session autocommit
assignments applied in order with the final value as session state, is limited
to row-DML transactions over routed MyLite tables. `libmylite` opens an outer
storage checkpoint for the transaction. Row-DML statements inside that
transaction use
`libmylite`-owned nested statement checkpoints so failed direct or prepared
statements can roll back their own partial writes while preserving earlier
transaction changes; the handler skips duplicate statement checkpoints while
an outer `libmylite` checkpoint is active. `COMMIT` or `SET autocommit=1`
releases the outer checkpoint, `ROLLBACK`
restores it, and closing a database handle with an active transaction
rolls it back before closing the embedded MariaDB connection.
Repeating direct or prepared `BEGIN` or `START TRANSACTION` while a transaction is
active commits the previous outer checkpoint and opens a new one, matching
MariaDB's transaction restart behavior for this bounded row-DML scope.
`START TRANSACTION READ WRITE` follows the same transaction-start path, and
`START TRANSACTION READ ONLY` starts a read-only MyLite transaction that rejects
direct and prepared MyLite storage writes. Direct and prepared
`SET TRANSACTION` and `SET SESSION` / `SET LOCAL TRANSACTION` `READ ONLY` /
`READ WRITE` forms mirror MariaDB's one-shot and session access-mode defaults
after MariaDB accepts the statement. Direct and prepared session
`SET TRANSACTION ISOLATION LEVEL ...` forms are accepted as compatibility setup
SQL after MariaDB validates them, but do not yet imply MyLite storage isolation
guarantees. Direct and prepared transaction read-only variable assignments
mirror the same session-default or one-shot read-only state; isolation variable
assignments are accepted as setup SQL without storage isolation semantics.
`COMMIT` and `ROLLBACK` completion
modifiers support `AND CHAIN` by finishing the current outer checkpoint and
immediately opening a new one; chained completion preserves the current
read-only/read-write access mode, while non-chained completion resets one-shot
access mode to the session default. `AND NO CHAIN` and `NO RELEASE` are
accepted explicit no-op completion modifiers. Direct and prepared session
`SET completion_type=NO_CHAIN/0/DEFAULT/CHAIN/1` mirrors the MariaDB
completion default so later plain direct or prepared `COMMIT` and `ROLLBACK`
follow the final supported assignment in a `SET` list, while explicit
`AND NO CHAIN` overrides chained defaults. Direct and prepared session
`SET autocommit` lists also allow duplicate supported assignments; MyLite
applies them in order after MariaDB accepts the statement and leaves the final
value as session state. Prepared single-marker values for supported session
`SET autocommit=?`, `SET completion_type=?`, transaction read-only, and
transaction-isolation assignments are validated before MariaDB prepared
execution and mirrored after successful execution.
`RELEASE`, `WITH CONSISTENT SNAPSHOT`, `completion_type=RELEASE/2`, global
transaction variables, global autocommit, direct-execution parameter markers,
expression-valued or global parameterized transaction-control `SET` forms,
bound `DEFAULT` / `RELEASE` transaction-control values, and duplicate
`SET TRANSACTION` characteristics remain unsupported.
Direct savepoint control is handled by `libmylite` before MariaDB execution
for the same bounded transaction scope: case-insensitive simple unquoted,
backtick-quoted, and ANSI_QUOTES double-quoted `SAVEPOINT` names open nested
storage checkpoint frames,
`ROLLBACK TO [SAVEPOINT]` restores the target snapshot and keeps the target
savepoint active, and `RELEASE SAVEPOINT` commits the target and later nested
frames while preserving changes. Prepared savepoint-control statements use the
same MyLite-owned checkpoint path and can be prepared before an active
transaction, but execution requires an active file-backed MyLite transaction.
Native MariaDB handler savepoint hooks are also installed for raw embedded
routed durable row-DML transactions, using nested MyLite storage checkpoint
frames while keeping metadata-lock release conservative.
Read-only transaction enforcement rejects direct and prepared durable MyLite
row writes before MariaDB execution, but permits simple single-target row DML
when the target name is tracked as a session-local temporary table. Explicit
direct and prepared `CREATE TEMPORARY TABLE` and `DROP TEMPORARY TABLE`
statements can execute inside active transactions because user
temporary-table rows, indexes, and autoincrement state live in process-local
volatile MyLite storage rather than durable primary-file pages.

Checkpoints save the committed header and catalog root pages while holding the
primary-file exclusive lock. Storage APIs in the same thread borrow that locked
descriptor only when the caller's storage context owner matches the checkpoint
owner. A different same-process `libmylite` handle reads through the saved
transaction-start header and catalog snapshot, so it sees committed rows,
row-state pages, index entries, and autoincrement state rather than the owning
handle's uncommitted appends. File-backed outer transactions also publish
`<database>.mylite-transaction-journal`, remove it as the durable commit point,
and use it to restore transaction-start visibility after an unclean process
exit. A cooperating process can read the same transaction-start snapshot from
that journal while the writer remains active; writes from other processes still
return busy under the coarse writer lock. If MariaDB reports statement failure,
MyLite restores the saved
catalog/header pages so rows, row-state pages, index entries, autoincrement
pages, and catalog records appended after the checkpoint are no longer visible.
After rollback finishes, the primary file is truncated to the restored header
page count, or to the later page count if rollback intentionally republishes
advancing autoincrement pages.
When the restored checkpoint is an outer durable transaction or a nested direct
savepoint frame, rollback scans appended autoincrement pages before restoring
the checkpoint and republishes only advancing values for table IDs that existed
in the checkpoint catalog. Durable generated insert paths can also mark their
current statement checkpoint after publishing an advancing autoincrement value
and before duplicate-key or foreign-key checks fail, so failed or ignored
generated inserts keep generated gaps without making failed DDL metadata
changes durable. Durable first-key generated inserts additionally publish and
mark the whole requested reservation interval from `get_auto_increment()`, which
preserves MariaDB/InnoDB-style failed multi-row insert gaps even when not every
reserved generated value is written. Successful explicit high-value inserts and
updates to first-key autoincrement columns publish the new lower bound through
the same ordinary autoincrement pages, so transaction rollback restores the row
image while preserving the advanced counter for tables that existed at the
checkpoint.

This is still partial SQL transaction support. The MyLite handler advertises
transactional table flags for MariaDB's bounded row-DML transaction capability
checks, while public `libmylite` SQL entry points continue to reject global
autocommit changes, unsupported `SET TRANSACTION` forms, unsupported
transaction modifiers, global transaction variables, direct-execution parameter
markers, expression-valued or global parameterized transaction-control `SET`
forms, bound `DEFAULT` / `RELEASE` transaction-control values, release
completion defaults, XA, and durable direct or prepared DDL inside active
transactions. Durable transactional DDL, full isolation-level semantics,
WAL/checkpoint, and broader native MEMORY/HEAP parity remain planned.

The storage design must preserve the full write-concurrency goal. Early
milestones may use coarse locks for correctness, but the page, transaction,
and lock manager designs must not bake in single-writer-only assumptions.

## Temporary Data

Temporary tables, query spill files, and recovery companions are storage policy,
not violations of the primary-file model.

- User temporary tables start as session-local state, do not publish durable
  catalog entries, and store rows plus indexes in MyLite's process-local
  volatile table store.
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
