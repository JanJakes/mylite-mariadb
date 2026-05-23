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

Active file-backed statements and transactions are storage checkpoints. The
storage layer keeps their mutable header state in memory and publishes page `0`
only at checkpoint commit, rollback, or savepoint propagation boundaries.
Ordinary active statements reuse one recovery journal for the checkpoint, while
active durable transactions use their transaction journal instead of creating a
normal recovery journal per row append. The handler treats an existing
THD-owned durable transaction checkpoint as proof that the storage statement is
already active, so repeated row-DML write locks inside the transaction do not
rediscover the same active storage chain by filename. Header reads inside
active checkpoints and transaction/read snapshots return the already-decoded
in-memory header instead of re-encoding and re-validating page `0`. Nested
checkpoints clone the parent checkpoint's current header/catalog snapshot under
the same owner and exclusive lock, so prepared row-DML savepoints do not
re-read or re-checksum a snapshot the parent already validated. Active
statements also cache validated catalog images by root page id and catalog
generation so row-DML and duplicate-key probes do not repeatedly walk or
checksum unchanged catalog metadata; catalog root writes and catalog-generation
header changes invalidate the active statement chain before later metadata
reads. Row append, update, delete, truncate, and autoincrement publication
paths publish decoded headers directly into the active checkpoint instead of
encoding and immediately decoding page `0`. Active checkpoints also keep a
transaction-local row-payload cache for indexed row reads, replacing cached
payloads after successful updates and dropping them after deletes, savepoint
rollback, truncate, or catalog invalidation. This lets repeated handler-driven
updates reuse the current row image without rereading and rechecksumming the
row page while preserving the same rollback and visibility rules. Row-payload
cache buckets use
tombstone-aware replacement and swap-removal, so changing a cached old row id to
its replacement row id does not rebuild the full bucket table on every update.
The active cache keeps a larger small-row working set, currently up to 32768
entries or 16 MiB of row bytes per table cache, and drops oversized rows rather
than retaining unbounded BLOB/TEXT payloads. A row retained in that active
payload cache is also accepted as the direct row-validation proof for the same
active checkpoint view, avoiding duplicate live-row validation bookkeeping after
indexed row reads. The hot row-update path passes its already-resolved active
payload cache into validation, so that proof does not rediscover the same
active cache owner per mutation.

Durable index cursor construction opens a scoped storage read session while the
handler finds matching index row ids and materializes the selected row payload
batch. The session holds one read-capable primary-file handle, keeps the shared
advisory lock for the cursor build, and reuses the validated header and catalog
root pages for repeated storage calls in that phase. This avoids reopening the
`.mylite` file and revalidating unchanged root pages for every point lookup
helper while preserving the same single-file visibility rules.
Exact and prefix handler reads that build a filtered cursor read the first
matching entry from that cursor directly; only range and boundary-oriented
read modes perform an additional ordered cursor search.

Read-statement startup also keeps a process-local checkpoint snapshot cache for
the current thread and storage owner. A new read statement reads the durable
header and catalog root bytes under the normal shared lock; when those bytes
match a previously validated snapshot, it reuses the decoded header/catalog
state instead of checksumming the same pages again. If either page differs,
normal validation runs and replaces the cache.
Short-lived read statements also reuse one cleaned statement object per thread.
The object reuse only avoids repeated allocation and cleanup of storage-layer
scratch state; it does not keep read locks open between SQL statements and does
not bypass recovery or checkpoint snapshot validation.
When the caller opens a matching filename identity scope, a read statement
borrows that stable filename pointer instead of copying it. Unscoped callers
keep owned filename copies, and the reusable read statement retains that owned
copy for the next same-file read statement while still replacing it when the
next read targets another file.

Durable full-row and count reads also keep a small process-local live-row-id
cache keyed by the durable header fingerprint and table id. When an unchanged
checkpoint has already produced a compacted live row-id list, later non-active
reads can reuse that list instead of scanning the append history and
rechecksumming every row and row-state page again. Mutations, truncate, catalog
changes, and cache-limit rotation clear the cache; active checkpoints and read
snapshots bypass it.
Active checkpoints can seed a complete live-row-id list from that durable
cache, maintain insert/update/delete changes in process memory, and promote the
updated list on top-level commit. Rollback, catalog changes, truncate, cache
overflow, or any inconsistent mutation state discards the active list and keeps
the existing append-history scan fallback. Complete live-row caches retain
amortized row-id capacity under their bounded entry limit, so maintained
inserts do not resize the row-id array for every appended row id.
Active exact-index caches follow the same transient handoff shape: a new active
cache can seed from a durable exact-index cache only when the filename, catalog
root, catalog generation, page count, table id, index number, and key width all
match the checkpoint header. Mutation hooks maintain already-loaded active
exact-index caches, and top-level commit promotes those complete maintained
caches under the committed header fingerprint. Rollback and cache invalidation
discard them instead of publishing uncertain duplicate-key or point-lookup
state. Exact-index cache entries are tombstone-aware and keep valid lookup
and row-id invalidation buckets across active row replacement and delete
maintenance when possible, with order-preserving compaction only after dead
entries outnumber live entries. In-place active rewrites that keep the same row
id skip exact-index cache maintenance for unchanged matching key images and
skip live-row cache retargeting because the row remains live under that id.
Row-only in-place rewrites also skip append-replacement live-row-id seeding and
unchanged-row exact-index retarget calls.
Existing active exact-index cache hits use a hot inline probe before the
storage layer enters the colder cache creation, durable seeding, and
append-history loading path.
Handler index cursor reads for non-BLOB rows validate the fixed payload length
and copy the stored record image directly, using the opened-handler BLOB/TEXT
metadata cache while BLOB/TEXT rows keep the full descriptor validation and
retained-payload ownership path.
Durable row-id, row-payload, exact-index, and published leaf-page caches are
retargeted after successful row insert, update, and delete when the mutation is
limited to a known table. Caches for that table are cleared or maintained by the
active checkpoint, while caches for other tables in the same file keep their
table-local contents and adopt the new committed header fingerprint. Catalog
changes, truncate, rollback, and cache-limit rotation keep broad file-level
invalidation. Deferred active-statement retarget markers store that compact
cache fingerprint rather than a full storage header. Indexed row materialization
resolves active and durable
row-payload cache availability once per batch and reuses durable cache pointers
while the cache-set generation is stable, avoiding per-row control-plane probes
when secondary cursors return many row ids. Handler cursor materialization also
reuses a handler-owned row-id scratch buffer, avoiding per-cursor allocation
when secondary exact reads repeatedly batch the same shape of row ids. Hot exact
indexed-row cache-hit paths skip catalog-image cleanup when no local catalog
image was allocated. Row-only update cleanup follows the same rule when
maintained index-root planning did not allocate a local catalog image.

The local storage performance baseline remains machine-dependent, but it now
accepts opt-in `--max-us=<metric>:<value>` thresholds so a slice can record an
explicit local regression gate without making default benchmark runs fail on
slower hardware. It also keeps quick positional runs capped while exposing
explicit `--profile-iterations=<n>` runs for longer local profiler samples.

MariaDB table-discovery callbacks also use scoped read sessions while they read
table definitions, discovered table names, or table existence from the MyLite
catalog. This keeps table-open discovery on the same cached checkpoint view as
cursor construction without extending read locks across the SQL statement.

The initial handler is opt-in. It is disabled in the default embedded baseline
and covered by a separate storage smoke build. That build verifies the
`MYLITE` row from `SHOW ENGINES`, explicit `CREATE TABLE ... ENGINE=MYLITE`
stores metadata in the primary `.mylite` catalog, and catalog discovery works
after close/reopen. When a MyLite primary file is active, MariaDB engine-name
resolution maps supported application engine requests such as `InnoDB`,
`MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, and `HEAP` directly to the MyLite
handler before native-engine fallback, including under `NO_ENGINE_SUBSTITUTION`;
the handler still records the requested engine name in catalog metadata. Routed
tables support row inserts, full scans, updates, and deletes from the primary
file, including BLOB/TEXT payloads. Supported
primary, unique, and secondary indexes append durable index-entry pages and
serve ordered handler cursors from MariaDB key tuples, including bounded
BLOB/TEXT prefix key images produced by MariaDB. Those cursors keep sorted key
metadata and row ids, then materialize the selected row payload lazily when the
SQL layer asks for a row. When a catalog-backed published leaf run exists for
a raw fixed-width key, full durable index cursor construction uses that
immutable run as the base snapshot and overlays later append-tail mutations,
rather than rebuilding the same index from the whole append history.
Single-level branch roots over those leaf runs choose exact-key and prefix
lower-bound child pages when the rebuilt snapshot fits in one branch page, and
readers follow the child page ids recorded in the branch cells rather than
requiring physically contiguous leaf pages.
When an insert first overflows a maintained single-page root and the live
entries fit in one branch, storage appends immutable leaf pages and rewrites
the same root page as a branch snapshot; later row DML against that index
uses branch maintenance for the currently supported shapes and remains visible
through the append-tail overlay for unsupported shapes.
Fitting inserts into existing single-level branch roots now rewrite the selected
leaf and branch page directly when the leaf has spare capacity, suppressing the
fallback index-entry page for that index. Inserts outside the current branch
high fence can also extend the last leaf and raise the branch high fence while
that leaf has spare capacity. When the final child leaf is full, the branch
root still has child capacity, and the existing branch tail has no live
row-state or index-entry overlay for that table/index, storage can keep the
existing final leaf full, append one new final leaf, and rewrite the branch
root with the new child. If live tail overlay exists but the refolded live
entryset plus the inserted entry still fits in one branch page, storage can
append a fresh leaf run and rewrite the existing branch root to point at that
new snapshot instead of hiding the overlay behind a moved branch tail. Other
full-leaf cases still use the append-tail overlay. Eligible deletes from the
final child leaf rewrite that leaf and refresh the final branch fence when the
branch still needs the same child count. When deleting the only entry in the
final child reduces the expected child count by one and another child remains,
storage now removes the final branch child cell and publishes the old leaf page
as a durable free-list run, coalescing when that leaf immediately precedes the
current free-list root run. Interior-child deletes remain on the row-state
overlay path until broader branch merge work exists.
Eligible updates whose source row is in the final child leaf and whose
replacement `(key, row_id)` still belongs in that final child now rewrite the
leaf and refresh the final branch fence without changing the branch entry
count; updates that cross child boundaries remain on the append-tail
replacement path.
`TRUNCATE TABLE` logically
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
multi-page transaction-aware index maintenance, free-space reclamation, and
unsupported index classes still reject
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
checksummed header and page 1 as the initial empty catalog root. Catalog
mutations publish a fresh catalog chain, repoint the header's catalog root to
the new chain, and then reclaim the previous non-active catalog-chain run into
a durable free-list page inside the primary file. Later non-active catalog
publication can reuse a suitable contiguous free run; active statement and
transaction checkpoints remain append-only until their rollback ownership is
extended. Catalog-keyed runtime caches are cleared after durable catalog
publication so reused catalog-chain pages cannot make stale page-count
identities look current. Row-payload cache entries carry an in-memory checksum
and are discarded before use when their copied bytes no longer match. Catalog
records remain page-local; a record larger than one catalog page payload is
still unsupported, while large table and FK payloads live in blob pages.
Explicit MyLite table definitions are stored as catalog records plus
checksummed definition blob pages. Schema namespace names use lightweight
catalog records; table-definition schema names are also treated as namespaces
for compatibility with files that pre-date explicit schema records. Internal FK
definitions use catalog records keyed by child schema/table and constraint name
plus typed FK blob pages for referenced key, column-list, action, match-option,
and nullable-column metadata. Catalog-backed index-root records key future
navigable index roots by schema, table, table id, and MariaDB key number; table
rename and drop preserve or remove those records with the owning table. FK
referenced-key rewrites append replacement FK blob pages and republish the FK
catalog record. Row inserts
append checksummed row pages tagged by catalog table id;
non-BLOB rows store raw MariaDB record images, while BLOB/TEXT rows store a
durable handler-owned row payload that replaces process pointers with value
bytes. Large row payloads spill into checksummed row-payload blob pages inside
the primary file. Update/delete appends checksummed row-state pages that hide
deleted or superseded row page ids; replacement row payloads are appended as
new row pages. Table scans validate those pages, filter hidden row ids, and
reconstruct MariaDB row buffers before returning them to the SQL layer.
In-memory row-state maps use transient hash buckets keyed by source row id, so
scans and index overlays do not linearly search every hidden row for each row
candidate after update-heavy workloads.
Full rowset reads collect live row ids in one file pass, compact those ids
against the row-state map, and then materialize only the surviving row pages.
Direct row-id reads validate the target row page first and scan only later
row-state pages that could hide that row, avoiding full row-state map rebuilds
for each selected index cursor row.
Autoincrement tables append checksummed state pages keyed by catalog table id so
generated values survive close/reopen and dropped table ids do not leak into
recreated tables. Supported primary, unique, and secondary indexes append
checksummed index-entry pages containing the catalog table id, MariaDB key
number, row page id, and MariaDB key-tuple bytes. Contiguous index leaf runs
can be rebuilt from live append-only entries, appended as root pages, and
catalog-published as exact byte-key lookup base snapshots. Single-level branch
roots over those runs store the total entry count in the branch page, with
zero-count legacy branch pages falling back to the catalog count. Exact lookup
derives the run length from root metadata or the branch page, searches leaf page
key ranges, scans only pages that can contain the requested key, then applies
later row-state and index-entry tail pages before returning matches. Handler
index reads build ordered in-memory cursors from live index entries and compare
keys with MariaDB's key helpers. Current mutating
publication paths are protected by a
rollback journal before the header or catalog root page is overwritten. Active
file-backed row-DML transactions create a transient transaction journal
with the transaction-start header, catalog root pages, and dynamically
protected dirty existing pages needed by current pager-backed maintained roots;
recovery applies any ordinary rollback journal first, then the transaction
journal. Richer transaction state, free-space metadata, and B-tree-style index
navigation are still planned slices.

MariaDB handler row statistics are estimates for durable MyLite tables. MyLite
does not advertise `HA_STATS_RECORDS_IS_EXACT`, and `ha_mylite::info()` answers
optimizer `HA_STATUS_VARIABLE` requests with nonzero stat-free estimates instead
of scanning every table row page or consulting the filesystem. SQL `COUNT(*)`,
scans, index reads, duplicate checks, FK checks, and the first-party storage
row-count API still use exact row visibility paths when they need exact
results. Volatile `MEMORY` / `HEAP` routed tables keep exact in-memory handler
counts because those counts do not touch the primary file.

Exact durable index reads also keep a small transient per-thread read cache for
lookups outside active MyLite storage checkpoints. The cache is tied to the
primary filename and observed header fingerprint, is cleared by durable writes
and catalog publications, and is skipped while active statements or transaction
snapshots are in scope. Published leaf roots remain the preferred path when
available; the cache amortizes repeated exact lookups over append-only indexes
that do not yet have maintained navigable pages. Cached exact-entryset reads
bulk-grow result arrays for all matching key images in one pass, so many-match
secondary reads do not pay per-row-id array reallocations. Exact-index caches
also maintain a transient bucket index for fixed-width key images, so repeated
point and exact-entryset probes walk only the matching hash bucket while
preserving cache order for collisions.

Active storage checkpoints maintain their exact-index caches across row updates
and deletes by removing hidden row ids and appending replacement row entries
instead of reloading the whole index after every mutation. Update/delete row
validation uses direct row-id visibility checks over later row-state pages, so
single-row DML no longer rebuilds a full table row-state map just to prove the
current row is live.

Durable handler index cursors materialize their matching row payloads in one
ordered batch after cursor construction. This keeps repeated secondary cursor
reads from reopening the primary file and revalidating header/catalog state for
each row while the storage layer still works from the same single `.mylite`
file and the same live row ids returned by index-entry reads.
Durable exact and full entryset reads also use scoped file/header setup,
matching primary-key point lookups and FK prefix probes when an active
statement, read statement, or snapshot already owns the current file view.
Full entryset reads then fall through to published roots or append-history
scans for ordered cursor construction.
Published leaf-root readers cache the last resolved index-root catalog entry
on the active statement, keyed by table identity, index number, catalog root,
and catalog generation, so repeated exact, full, and prefix probes do not
rescan the catalog image for the same root metadata.
Full-row scans, exact row counts, direct row reads, and indexed-row batch
materialization also use scoped file/header setup, so those row-oriented APIs
reuse active statement views before consulting live-row and row-payload caches.
Schema/table discovery reads, schema/table listing, and foreign-key listing
also use scoped file/header setup before reading the catalog image.
Foreign-key definition reads use the same scoped setup before reading catalog
records and FK metadata BLOB pages.
Header validation, index-root metadata, and durable autoincrement metadata
reads use the same scoped setup before validating catalog/free-list roots or
resolving scalar metadata.
Residual exact point lookups and broad index-prefix existence scans also close
or execute through scoped file lifecycles, matching index-specific prefix and
entryset reads.
Schema namespace catalog writes use scoped update file/header setup before
publishing catalog mutations, including the no-op existing-schema path.
Index-root catalog writes use the same scoped update setup before publishing or
removing root metadata records.
Foreign-key metadata writes use scoped update setup before publishing,
renaming, or dropping FK catalog records and metadata BLOB pages.
Durable autoincrement set and advance writes use scoped update setup before
resolving table metadata and publishing scalar autoincrement pages.
Table catalog writers use scoped update setup before publishing table
definition BLOB pages, FK-aware drops, or table rename metadata.
Index leaf rebuild publication also uses scoped update setup before writing
rebuilt leaf pages and catalog root records.
Row append, delete, and truncate mutation paths use scoped update setup before
writing row pages, row-state pages, maintained index roots, and truncate
autoincrement reset pages; the generic update-open wrapper has been removed.

File-backed index cursor builds also keep the primary file open across exact
lookup and row materialization. Primary-key point lookups and secondary exact
cursor builds reuse that scoped file view and cached header/catalog images.
Single-entry exact unique cursors keep their copied key, cursor entry, and
one-row materialization offsets inline in the handler when the key fits
MariaDB's normal key buffer, avoiding per-lookup heap allocation on primary-key
point reads. They also materialize the selected stored row into a handler-owned
scratch buffer that grows only when needed, so fixed-record point updates do not
allocate and free a new serialized row payload for every lookup. Active
same-owner checkpoints also cache the current table's catalog entry by schema,
table, catalog root page, and catalog generation, so repeated exact cursor
builds can reach the active exact-index cache without recopying and rescanning
the catalog root. The same cache serves active row-write table-id resolution,
so repeated inserts, updates, deletes, truncates, and autoincrement operations
on the same table do not rescan catalog metadata while the catalog fingerprint
is unchanged. If a same-owner write checkpoint is already active, reads use the
write checkpoint's current view instead of opening a separate read session.

Repeated read statements over an unchanged file also reuse a thread-local
decoded checkpoint snapshot after comparing the raw header page. A byte-identical
header is enough to reuse the cached catalog snapshot because the header records
the catalog root and generation; the cache is also tied to the database file's
device and inode so path replacement with a separate file cannot reuse a stale
catalog snapshot. This keeps repeated point-select statements from paying
header/catalog checksum costs when no storage page publication changed the
durable view.
Normal read statements also keep a thread-local unlocked read file handle after
the read statement ends. The next read statement can reuse that handle only
after confirming the filename still resolves to the cached device and inode,
then it takes the ordinary shared lock and reads the checkpoint snapshot as
usual. The cache records the handle identity when it is stored, so reuse does
not repeat `fstat()` on every read statement. The cache is cleared on same-file
creation and durable mutation paths.
The statement object itself is also retained after cleanup, bounded to one
object per thread, so hot point-read loops do not allocate the large
`mylite_storage_statement` scratch structure on every short read scope.
Read statements under a trusted filename identity scope borrow the scoped
filename pointer for the same active lifetime; raw callers without that scope
keep the owned-string path.
Read-statement startup also keeps cached deterministic recovery and transaction
journal path strings for the current filename. It still checks both journal
paths on every startup before taking a shared read lock, preserving
cross-process recovery behavior without rebuilding those path strings for hot
read loops.
Random page reads and writes use offset-addressed `pread()` / `pwrite()` calls
instead of moving stdio stream position for each fixed-size page. Sequential
stream writes remain limited to empty database initialization and rollback
journal construction.

Catalog discovery for table-open flows uses the same scoped read-session cache,
so repeated prepared statements avoid a separate header/catalog validation path
before the handler reaches the index cursor.
For exact unique-key prepared point updates, the MyLite handler can accept
MariaDB's direct-update hook and materialize the target row directly through
the exact-index path before applying MariaDB's normal condition, assignment,
constraint, no-op, and handler update semantics. Direct admission stays narrow:
unique-key-changing updates and key changes on FK-involved tables fall back to
the ordinary update loop, while non-unique secondary-index changes reuse the
existing index-maintaining update path. Accepted changed rows call MyLite's
row-update implementation directly from the direct-update hook and then run the
handler-owned hidden-index/stat side effects that the generic wrapper would
normally provide. Tables with long-unique hash or `WITHOUT OVERLAPS`
constraints still fall back to MariaDB's ordinary update loop because their
in-server update checks are private to the handler wrapper.

Active storage checkpoints also maintain a live-row validation cache per table
and catalog generation. Rows proven live by visibility-checked storage reads,
index-entry reads, exact-index lookups, and table scans can be trusted by later
update/delete validation without rescanning later row-state pages. Rows whose
payloads were read, or whose row pages were just written by a successful
mutation, also carry a stronger table-validation proof, so later validation can
skip rereading the old row page. Successful mutations remove hidden source row
ids and add replacement row ids. Successful durable updates also seed the
active checkpoint's row-payload cache with the replacement row when the update
entered without an existing payload entry, so repeated updates inside the same
outer checkpoint can validate from active cache state. Non-checkpointed direct
storage calls keep the row-page and row-state scan path, and truncate/catalog
invalidation or savepoint rollback clears cached live rows.

Small inline row updates append the replacement row page, row-state page, and
replacement index-entry pages as one contiguous page run. The durable page
format, page order, and FNV checksum values stay unchanged. Fresh page encoders
hash the meaningful prefix as checksum-free spans and mathematically skip the
known-zero tail, while decode paths still verify the full page with the same
span-based checksum stream so corruption in unused bytes remains detectable.
Large overflow payload updates keep the existing per-page writer until blob
payload batching has its own design. Active checkpoints keep a
bounded transient append-page buffer for those contiguous unpublished page
runs. The current performance profile uses a 32768-page window, which is
128 MiB at the 4096-byte page size. Nested statement commits inside a durable
transaction can accumulate into the outer checkpoint, readers in the same
checkpoint consult the buffer before the primary file, top-level commit flushes
it before publishing page `0`, and rollback flushes any retained prefix before
truncation while discarding pages past the restored checkpoint.
Durable updates compare MariaDB's old and new serialized key images and omit
replacement index-entry pages for unchanged keys. Exact-index, live-index, and
published-leaf tail overlays inherit omitted unchanged entries through the
row-state replacement id, while later physical replacement entries for changed
keys supersede the inherited entry. This reduces common non-key update write
volume without changing the durable page format. The handler uses the same
changed-key vector to skip duplicate-key probes for unique keys whose serialized
image is unchanged, while inserts and changed unique keys keep the normal
duplicate checks.
If an active update targets an inline replacement row whose replacement page run
is still resident in the active append-page buffer, the storage layer rewrites
that buffered row page and any changed matching index-entry pages instead of
appending another replacement chain. This works across nested statement frames
inside the same outer checkpoint by capturing per-statement buffered-page
preimages before rewriting pages that predate the current savepoint. The
row-state page remains unchanged, the row id is reused, and rollback restores
captured buffered preimages before truncating or flushing the retained prefix.
The buffered rewrite preflight uses checksum-free metadata validation only for
unpublished in-memory row and index pages. Row-state pages are fully
checksummed the first time a buffered replacement row is rewritten, then that
validated row id is cached in a hash-backed set on the append-buffer owner so
later rewrites can use metadata-only row-state validation until rollback or
statement cleanup clears the cache. After the buffered row, row-state, and
small changed-index page shape is validated once, later matching rewrites can
skip repeated metadata decodes until rollback, statement cleanup, append-buffer
miss, or changed-index shape mismatch returns them to the validated fallback
path. After validation, the rewrite mutates the row page and changed
index-entry pages directly in the active append buffer, refreshing only the
mutable payload/key bytes and any stale shrunken tail, and capturing
per-statement preimages first when rollback needs them. Row and index-entry
preimages store only the meaningful checksummed prefix plus an implicit zero
tail, while other page types keep full-page undo. The active row/index rewrite
path passes typed prefix sizes into undo capture instead of rediscovering the
page type from the full preimage. Row-only rewrite calls reuse the buffered
row/state page references that the rewrite dispatcher already resolved instead
of probing the append buffer again for the same pages. Successful statement
cleanup can retain one
small thread-local undo-list allocation for later statements, but active
statements never share mutable undo storage. Rewritten buffered row and
index-entry pages mark their checksums dirty and refresh them only before a
generic checksum-validating read or append-buffer flush, so repeated in-memory
rewrites do not rehash the same pages on every update. The same rewrite path
resolves the append-buffer owner once and passes that statement through local
page, range, undo, and dirty-flag helpers instead of scanning active statements
for every buffered-page access.
The generic buffered read and write helpers still copy pages for other callers,
and durable reads keep full page checksum validation. Already-flushed
replacement runs keep the append-only path until a logged page-rewrite design
exists.

Non-active durable indexed-row reads use a bounded thread-local row-payload
cache keyed by the primary file header fingerprint and table id. Repeated
secondary cursor materialization can reuse payloads that were already read and
checksummed for the same durable file state. Each cache keeps an in-memory
row-id index so cache hits stay near constant time. The cache is disabled for
active statements and read snapshots, and it is cleared by durable mutation
invalidation. Indexed-row batch materialization resolves the durable
row-payload cache once per row-id batch and reuses it for hits and fills while
the cache set generation stays stable, then builds rowsets through an internal
builder that preallocates metadata for the known row-id batch and grows row
bytes amortized, avoiding per-row cache discovery and metadata reallocations.

Non-active durable index-leaf reads use the same file header fingerprint model
for a bounded thread-local leaf page cache. Published leaf-root exact lookups
can reuse validated leaf pages without repeating the file read, checksum, and
decode work for every cursor build. When a published run has no append tail,
single-row exact probes return the first matching row id directly from the leaf
page instead of allocating a temporary row-id list. Exact leaf-page matches
bulk-grow their index entryset result arrays and row-id lists once per matching
leaf page instead of reallocating per matched row id. Internal row-id result
lists also retain amortized capacity, so append-history exact scans and
append-tail overlays do not resize the row-id array for every incremental
match. Leaf-run exact and full-read helpers reuse the validated root page from
run discovery for page offset `0`, avoiding redundant leaf-page cache lookups
for single-page published roots. The cache is disabled for active statements and
read snapshots, and it is cleared by durable mutation invalidation.
Durable foreign-key prefix checks use an index-specific storage helper that can
answer directly from complete static published leaf roots and single-page
maintained roots. Roots with append-tail history, missing roots, and unsupported
shapes still fall back to the existing materialized index-entry overlay path.
Static prefix probes lower-bound within each sorted leaf page, including probes
whose serialized prefix is shorter than the full key image. Prefix probes also
use scoped file/header reads, matching exact-index point lookups when an active
statement, read statement, or snapshot already owns the current file view.

MariaDB handler instances also cache proven child and parent foreign-key
metadata absence for their opened table. Ordinary non-FK row-DML paths use that
cache to avoid repeating catalog-listing work before every write, update,
delete, or parent-reference probe, while tables with matching FK metadata keep
the existing enforcement and action paths. Successful local table-DDL mutations
bump a process-wide FK metadata epoch so already-open handlers refresh cached
presence results before later DML.

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
remain orphaned inside the primary file until broader free-space management
exists; superseded catalog-chain pages are reclaimed into the catalog free-list.
New table ids are allocated above both live catalog records and existing row
pages so drop/recreate does not expose old rows. Simple
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
not as Aria tables in a datadir. File-backed `libmylite` sessions therefore
default `use_stat_tables=NEVER`, so ordinary query planning does not repeatedly
open inherited persistent `mysql.*_stats` tables that are not MyLite-owned
state.

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
Grouped later-in-key MEMORY/HEAP autoincrement reads matching live volatile
index entries by serialized prefix before applying the same handler-side
maximum selection as durable grouped allocation.
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
Grouped later-in-key transaction and savepoint rollback follow the same
live-prefix model: rolled-back generated rows and rolled-back explicit high
rows are removed from row/index visibility, so the next generated value is
computed from the current live maximum for that prefix. Representative
coverage includes direct transaction rollback, nested savepoint rollback,
prepared insert rollback, explicit routed `ENGINE=InnoDB`, and close/reopen.
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
The durable grouped autoincrement path reads live entries for the current
serialized key prefix through a storage prefix-entryset helper, so published
leaf roots can skip unrelated base entries while preserving row-state overlay
semantics. Append-tail pages are still scanned until storage-level B-tree
navigation exists.
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
row-page validation until a selected row is materialized. Direct selected-row
materialization validates the requested row page by id and scans only later
row-state pages for a hiding state. Ordered index reads
build in-memory cursors from live index entries, keep sorted key metadata plus
row ids, use bound searches for key positioning, build filtered cursors for
exact-key and prefix reads, stop after the first matching non-nullable full-key
unique integer entry, and use storage-level exact-entry or exact-entryset lookup
for guarded raw equality paths so the handler does not allocate unrelated index
entries for common integer point reads. Storage prefix-entryset reads similarly
return only live entries matching a serialized key prefix for durable grouped
autoincrement allocation, and durable prefix-existence fallback checks reuse
an allocation-free row-id overlay instead of materializing full or narrowed key
entrysets.
Static no-tail prefix-existence checks use leaf-run page bounds before the
existing allocation-free page-local prefix check.
Durable exact lookups classify each
published append-only page once and prune candidates as later row-state pages
hide older row ids. Contiguous index leaf runs can serve as exact and prefix
lookup base snapshots by searching run page key ranges, or by using a
single-level branch root's high key fences and recorded child page ids when one
is published, with only pages appended after the highest branch child page
scanned as a visibility overlay; missing roots fall back to the append-only
scan path. Branch roots now report their page-owned entry counts for metadata
reads when present, while zero-count legacy branch pages keep using the catalog
record count. Eligible final-child deletes can physically remove the leaf entry
and refresh the final branch fence while preserving the branch-root invariant
that every non-final child leaf remains full. Eligible final-child updates can
physically replace the leaf entry and refresh the final branch fence when the
replacement entry remains in the same final child. Eligible final-child
removals can drop the final branch child when the branch child count decreases
by one and reclaim the removed leaf page through the durable free-list,
including coalescing when the removed leaf immediately precedes the current
free-list root run. When
that removal leaves a live entryset that fits in one maintained root page,
storage collapses the branch root back to the maintained root format by
materializing live branch entries and existing tail overlays before applying
the current delete.
Copy-rebuild DDL publishes supported fixed-width leaf roots for every current
key that fits the raw format in the rebuilt table, including retained primary
keys after forced rebuilds, by scanning the append history once for the table's
raw key set and publishing one catalog update. Pure table renames keep existing
root metadata without rebuilding. Cursors check
`index_next_same()` boundaries before row materialization and reconstruct only
the selected row buffer from row pages. Active checkpoints reuse statement
journals, defer header publication to checkpoint boundaries, and cache guarded
exact duplicate-key probes on the outer active checkpoint so nested libmylite
statement checkpoints do not force full exact-index scans for every row insert.
Nested checkpoint rollback invalidates parent exact-key caches before restored
header state is propagated. Handler index cursors materialize row ids returned
by storage-filtered index entrysets through an indexed-row read path that
validates the row page and table id without repeating the row-state visibility
scan. This provides correct indexed insert, lookup, update, delete, reopen, and
copy `ALTER` behavior for the supported shapes, but it is still an interim
performance structure because maintained B-tree navigation and pager-style write
paths are not implemented. A first pager foundation now wraps fixed-page access
for row-page, row-state, autoincrement, BLOB payload, free-list, append-only
index-entry, and index leaf access, but dirty-page ownership and rollback for
maintained B-tree updates remain planned. Versioned rollback journals can now
protect a bounded set of typed storage pages, giving the pager a recovery
boundary for future dirty in-place page writes. Active dirty-page rollback now
captures full-page preimages for existing pages dirtied through the pager,
merges nested savepoint preimages upward on release, and restores them on
rollback before truncation. The first dirty existing-page write in an ordinary
active statement now creates a recovery journal that protects that page before
the write reaches the primary file; dirty writes that cannot be protected under
the current immutable journal model are rejected instead of becoming
crash-unsafe. Active statements can also create the immutable recovery journal
from a bounded preplanned dirty-page set, which lets future maintained-index
writers declare root or leaf pages before row append creates the journal.
Maintained index root pages now have a single-page typed format with
root-owned entry counts, fixed key width, flags, used bytes, and sorted
`(row id, key bytes)` cells. Small non-empty fixed-width rebuilds publish that
root page type, and full/exact index readers dispatch by root page type so
maintained roots use their own entry count while oversized rebuilds continue to
use immutable leaf runs. Index-root metadata reads also decode maintained root
pages for current entry counts, so later in-place updates do not need catalog
publication solely to keep metadata counts fresh. Eligible inserts now plan and
journal single-page maintained root rewrites before writing the row page, insert
the new key in sorted order, and skip the duplicate append-only index-entry
page. Rebuild scans also treat maintained roots as live index sources so a
later rebuild or rename does not lose entries that were inserted directly into
the root. Eligible deletes now physically remove the source row id from
maintained roots while preserving the row-state delete overlay for immutable
and fallback paths. Eligible updates now replace source-row cells in maintained
roots with the replacement row id and new key bytes, keep root entry counts
stable, and skip duplicate append-only replacement index-entry pages for roots
updated in place. Maintained roots mark overflow-tail history in the existing
root flags field when a full root falls back to append-only index-entry pages.
New overflow roots also store the first fallback index-entry page id when it is
known, letting readers skip first-overflow row payload pages; older overflow
roots with no stored tail start still fall back to `root_page + 1`.
Maintained-root insert, update, and delete paths reject unsafe plans when dirty
root pages cannot be protected by the statement or transaction journal, rather
than publishing append-only fallback state that root-backed readers cannot see.
Root-backed exact and full index readers use maintained roots as base entries
and scan the append tail only when the overflow flag is set. Root-resident
mutations suppress duplicate append-only index entries, so the tail overlay
keeps fallback history visible without adding stale root entries. Root-resident
deletes also refill an overflow-marked root from the live root-plus-tail entryset
when all remaining entries fit in the single-page root again, clearing the
overflow flag and avoiding stale tail scans.
Statement-owned recovery journals can now be rewritten to protect late dirty
pages introduced after the journal was first created. This keeps maintained-root
updates recoverable when another page owner, such as autoincrement metadata,
opens the recovery journal before row append reaches the index root.
Active exact-index and published-root cache reads bypass stale durable cache
state while an active statement chain has deferred table-local durable cache
retargeting. Statement rollback, savepoint rollback, transaction rollback,
stale statement-journal recovery, and stale transaction-journal recovery now
restore single-page maintained root bytes and logical visibility for covered
insert, update, and delete paths, and restore covered single-level branch
final-leaf deletes, updates, final-child removals, and final-leaf free-list
publication. Root splits, multi-page navigable indexes, branch-page
split/merge, child-boundary updates, broader free-list run coalescing, and
broader transactional maintained index mutation remain planned.
Standalone
`CREATE INDEX` and `DROP INDEX` are covered for supported copy-rebuild index
definitions. B-tree pages, row/index free-space reclamation, and broader
multi-statement transaction-aware index maintenance remain planned.

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
append-only mutations. Version `2` journals store the committed header page and
a bounded set of protected typed storage pages; version `1` journals with the
older three-page header layout remain readable for pending-journal recovery.
Current catalog mutations protect the committed catalog root page. When catalog
publication will reuse the catalog free-list, the journal also protects the
current free-list root page before that node is mutated. Catalog overflow pages do not
need separate journal slots because catalog publication writes a fresh chain
before repointing the header and reclaims the previous chain only after the
catalog publish journal is removed. It is fsynced before primary-file writes,
the primary file is fsynced before the journal is removed, the parent directory
is synced after journal create/remove, and every storage open first recovers and
removes a valid pending journal. Recovery restores the previously committed
header/catalog/free-list-root state and truncates the primary file to the
restored header page count. Committed orphan pages from row, index,
update/delete, and truncate history still wait for broader free-space
reclamation. Physical write,
flush, sync, and truncate paths classify no-space, quota, and file-size-limit
failures as `MYLITE_STORAGE_FULL`, leaving generic I/O errors for non-capacity
failures.
The MariaDB handler maps that storage result to a file-full condition rather
than reporting a crashed table or index.

Storage operations now use advisory locks on the primary `.mylite` file
descriptor. Read APIs take a shared lock after pending recovery is handled,
while writes and recovery take an exclusive lock before mutating or restoring
pages. Conflicts return busy errors after the current thread's configured busy
timeout expires; a zero timeout keeps immediate non-blocking behavior. No
durable lock sidecar is created. These locks protect cooperating MyLite
processes from unsafe concurrent access but are not the final multi-writer lock
manager. Scoped handler cursor builds can keep a shared lock across the exact
index lookup and row materialization helpers rather than for each individual
storage helper call. If a cooperating writer holds an active row-DML
transaction lock and has published a valid transaction journal, cross-process
storage reads use the journal's transaction-start header and catalog root pages
as a read-only snapshot; stale journals without an active exclusive writer
still require the exclusive recovery path.

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
When active append pages are still buffered in process memory, rollback first
flushes any buffered prefix that belongs before the restored page count, drops
buffered pages after the restored checkpoint, and then uses the same primary
file truncation rule. That keeps savepoint rollback deterministic even when a
successful earlier statement in the same transaction has not yet forced its
append run to disk.
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
Grouped later-in-key generated values do not rely on that scalar counter for
allocation; after rollback restores row/index visibility, the next
`get_auto_increment()` call recomputes from live prefix entries.

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
