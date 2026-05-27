# Storage Architecture

MyLite's storage architecture uses MariaDB native storage engines inside one
MyLite-owned database directory. MariaDB provides SQL parsing, metadata
semantics, optimizer integration, expression evaluation, diagnostics, handler
calls, native engine file formats, transactions, recovery, and engine-level
concurrency. MyLite owns the embedded startup contract, directory lifecycle,
configuration, unsupported server-surface policy, and tests that prove files
stay within the documented boundary.

## Product Invariant

"Single directory" means one user-managed database directory, not a daemon-owned
server installation.

- The portable durable asset is one directory, such as `app.mylite/`.
- New database directories should use the `.mylite` suffix as a convention,
  for example `app.mylite/`. MyLite should not reject other directory names
  unless a later compatibility slice adds an explicit migration or policy check.
- Durable MariaDB data, metadata, engine files, logs, locks, journals, and
  recovery state stay inside the MyLite database directory.
- Durable files outside the MyLite database directory are outside the final
  product shape.
- MyLite-owned temporary or runtime paths outside the directory are allowed only
  when explicitly documented as transient, deterministic, and cleaned up by the
  MyLite lifecycle.
- Recovery files left after an unclean shutdown are part of the MyLite database
  directory lifecycle, not separate user-managed database assets.

## Architecture Decision

MyLite keeps MariaDB native storage engines. It does not implement a custom row,
index, pager, catalog, or transaction format as the final storage direction.

Reasons:

- Native engines inherit MariaDB file formats, SQL behavior, transaction
  behavior, recovery behavior, and write-concurrency semantics.
- Using native engines is substantially smaller and lower risk than
  reimplementing rows, indexes, constraints, transactions, and crash recovery.
- The long-lived fork delta stays focused on embedded startup, directory
  ownership, configuration, unsupported server surfaces, and public API shape.
- Compatibility work can validate real `ENGINE=InnoDB`, `ENGINE=MyISAM`, and
  `ENGINE=Aria` behavior instead of emulating those engines behind a custom
  storage layer.

## Directory Layout

The first durable layout makes the directory roles explicit:

```text
app.mylite/
  mylite.meta
  mylite.lock
  datadir/
  tmp/
  run/
  concurrency/
    mylite-concurrency.meta
    mylite-concurrency.lock
    mylite-concurrency.shm
    mylite-concurrency.wal
    mylite-concurrency.ckpt
```

- `mylite.meta` records MyLite directory version, base MariaDB version,
  lifecycle flags, and compatibility metadata that MyLite needs before starting
  the embedded runtime.
- `mylite.lock` is a MyLite-owned advisory lock anchor for cross-process
  directory ownership. It may remain after clean or unclean shutdown.
- `datadir/` contains MariaDB-native database directories, metadata files,
  engine files, redo/undo/log files, and other durable storage-engine state.
- `tmp/` is the root for per-runtime MyLite-owned temporary files and query
  spill that must not escape the database directory.
- `run/` is the root for per-runtime lock files, process-local markers, or
  runtime state that is safe to remove after clean shutdown.
- `concurrency/mylite-concurrency.meta` records the durable identity and
  generation seed for future ownerless coordination. It does not enable
  shared read-only or ownerless read/write opens by itself.
- `concurrency/mylite-concurrency.lock` is the future byte-range lock anchor.
  The current exclusive mode uses its `PERSISTED_CONFIG`, `RECOVERY`,
  `SHM_RESIZE`, and `OPEN_REGISTRY` ranges while creating or validating
  concurrency metadata, shared-memory layout, stale `.shm` rebuilds, and the
  process and transaction registries. Shared-memory preparation takes
  `RECOVERY` before `SHM_RESIZE`, matching the planned global ownerless lock
  order.
- `concurrency/mylite-concurrency.shm` is a grow-only file-backed shared-memory
  file. It starts with a fixed 128-byte MyLite header containing a magic value,
  format markers, byte-order marker, clean/dirty/rebuilding state, mapping
  size, generation counters, segment-table metadata, and the database UUID from
  `mylite-concurrency.meta`. The first segments are a fixed process registry,
  fixed wait-channel table, fixed MDL lock-table foundation, fixed
  transaction-registry foundation, fixed read-view registry, and fixed InnoDB
  lock registry; exclusive opens map the file, mark `.shm` dirty, allocate one
  active process slot for the embedded runtime, clean dead-owner MDL,
  transaction, read-view, InnoDB-lock, and redo-visibility entries, and release
  that slot before marking `.shm` clean on final close. Clean opens preserve
  the existing registry, wait-channel, MDL lock-table, transaction-registry,
  read-view, and InnoDB-lock segments. A later open treats dirty or rebuilding
  `.shm` state as
  stale volatile coordination state, rebuilds those fixed foundation segments,
  and increments the recovery-generation field. The file is created at a
  minimum size for future coordination, is validated through a
  `MAP_SHARED` mapping during durable opens, is never shrunk by open, and stale
  or invalid header bytes are rebuilt because the `.shm` file is not durable
  truth. MyLite has fixed-width owner-generation-aware latch words over its
  mapped wait backend, an internal ownerless platform probe, internal
  process-slot allocator, internal metadata lock-table primitive with
  repeated-owner reference counts, same-owner mode upgrades, MariaDB-style
  granted compatibility for schema and table MDL modes, dead-owner cleanup,
  stable schema/table MDL key hashing, and an
  internal transaction-registry primitive for cross-process monotonic
  transaction IDs, active-ID snapshots, oldest-active tracking, stale end
  rejection, and dead-owner transaction cleanup. The transaction registry is
  wired into the production `.shm` layout and process-slot cleanup path.
  MariaDB's embedded MDL ticket lifecycle now has a MyLite hook surface for
  schema/table lock acquire and release balancing, including cloned tickets,
  upgrades, and downgrades. The production `libmylite` runtime registers that
  hook against the directory-backed MDL lock-table segment using the runtime
  process-slot owner while the current exclusive directory lock is still held.
  InnoDB's transaction-system boundary now also has a guarded MyLite hook
  surface for maximum transaction ID reads, transaction ID allocation, active
  read-write registration, transaction serialisation-number assignment,
  active-ID snapshots, and deregistration, with embedded SQL coverage proving
  ordinary InnoDB statements reach those hooks under test. InnoDB read views
  are also published into the directory-backed read-view registry and removed
  on close. InnoDB table and record locks are mirrored into the
  directory-backed InnoDB lock-registry segment for granted native locks,
  including locking-read paths that acquire locks before `trx_t::id` exists,
  with commit, rollback, close, and dead-owner cleanup coverage. Local InnoDB
  table and record waits are now published as directory-owned wait edges and
  cleared when InnoDB resets the waiting lock. InnoDB table and record grant
  paths now reserve a shared registry entry before creating or extending a
  granted native lock. A conflicting external registry entry now leaves the
  native lock request waiting, publishes a directory-owned wait edge, sleeps on
  the mapped registry wait word without holding InnoDB local latches, refreshes
  redo visibility after wake, retries native grant, maps shared-registry
  timeout to the normal InnoDB lock-wait error, and maps cross-process wait
  cycles to the normal InnoDB deadlock error under guarded SQL coverage. Before
  ownerless write locks are released on commit, dirty pages are flushed through
  the transaction commit LSN so the current test-gated visibility bridge does
  not need a whole-buffer-pool sync for every commit. Redo append ranges are
  reserved through short owner-generation-aware shared-memory critical sections,
  with active reservation records kept as recovery evidence while writes are in
  flight; the short progress latch is also recovery-sensitive if its owner dies
  mid-bookkeeping. Ownerless durable page-write coordination now covers all
  durable in-file InnoDB pages, and undo tablespace allocation currently flushes
  the touched undo space before releasing the shared allocation lock so a peer
  does not reuse stale free-space metadata. Autocommit ownerless statements
  refresh local InnoDB redo/page state to the latest shared visibility before execution,
  while explicit transactions avoid global refresh and rely on targeted
  post-wait page refresh. The same shared-memory layout now contains a
  fixed page-version index segment that points current guarded autocommit page
  reads at WAL record offsets instead of scanning the whole page-version payload
  on the normal path. Ownerless SQL opens serialize core
  `mysql.*` compatibility-table bootstrap through `mylite-concurrency.lock` so
  two processes do not both run Aria-backed `CREATE TABLE IF NOT EXISTS` on the
  same system tables during open. The same directory-owned lock byte serializes
  ownerless embedded runtime bootstrap, because InnoDB startup takes internal
  table locks before user SQL begins. Recovery decisions read volatile
  process-registry counters through `MAP_SHARED` mappings instead of ordinary
  file reads, so a live peer's slot cannot be missed by stale file-cache state.
  If volatile `.shm` state must be rebuilt, the page-version index segment is
  initialized from the durable page-version records in `mylite-concurrency.wal`.
  The embedded runtime disables InnoDB buffer-pool dump/load so concurrent
  processes do not race on the advisory `ib_buffer_pool` file in `datadir/`.
  Ownerless read/write is available through `MYLITE_OPEN_OWNERLESS_RW` for the
  tested InnoDB concurrency subset, while broader DDL invalidation,
  transaction-aware page-version reads, retained-record checkpoint replay, and
  long-running stress remain planned.
- `concurrency/mylite-concurrency.wal` and
  `concurrency/mylite-concurrency.ckpt` are durable coordination-log and
  checkpoint anchors for future ownerless recovery. They contain fixed headers
  with magic, format, byte-order marker, generation, and the database UUID. The
  `.wal` header is followed by a first-party page-version log payload whose
  primitive covers fixed record encoding, byte-range serialized cross-process
  appends, direct record-offset reads, latest-visible page lookup by commit LSN,
  payload offsets, and incomplete-tail tolerance. Production ownerless runtimes
  initialize that payload path, and guarded ownerless InnoDB commits append
  dirty page images up to the transaction commit LSN before the current
  conservative flush bridge releases shared lock-registry entries. Guarded
  ownerless autocommit `SELECT` reads can substitute non-system InnoDB
  tablespace pages from the page-version payload through the shared page-version
  index after refreshing to the latest page-visible commit LSN. The page-visible
  LSN advances only after dirty pages for that commit have been published and
  flushed through the current conservative bridge, so a raw redo LSN cannot make
  page-version reads trust an incomplete commit. The `.ckpt` anchor persists the
  latest raw redo LSN and page-visible LSN, and `.shm` rebuild seeds the redo
  state segment from that durable record instead of resetting peer visibility to
  zero after a dirty shared-memory recovery. Page-visible publication is clamped
  to contiguous completed redo writes so a later writer cannot expose pages past
  an earlier unwritten redo gap. The same page-version log can
  replay record offsets into a rebuilt `.shm` page index, so deleting or
  discarding closed volatile shared-memory state does not lose the guarded
  autocommit lookup path. The page-version read window is scoped to the
  executing SQL thread so one embedded handle cannot leak page visibility into
  another handle in the same process. If the bounded shared-memory index fills,
  guarded reads fall back to the page-version WAL scan rather than trusting
  stale indexed offsets; WAL scans capture a stable log-end snapshot before
  walking page records so writers are not blocked for the full scan. Guarded
  page reads hold the page-log checkpoint read lock while resolving shared-index
  offsets, so checkpoint truncation cannot move a page record between index
  lookup and direct read. The page-version log primitive can compact records at
  or below a safe commit LSN while retaining newer records; the product
  checkpoint path uses a narrower safe truncation that only removes the log
  payload when every complete record is already at or below the page-visible
  LSN. Before truncation, it invalidates indexed WAL offsets; after a successful
  empty-log checkpoint, it clears the page index so future page publishes can
  use indexed lookup again. Active transactions, DML/DDL, prepared execution,
  system tablespace pages, tablespace replay, and retained-record checkpoint
  rewrites still do not consume that index. Ownerless InnoDB purge-history
  truncation and history freeing are currently
  disabled until purge oldest-view and undo-free coordination are moved into the
  directory-owned protocol.

The native-storage baseline starts MariaDB with `--datadir=app.mylite/datadir`,
`--tmpdir=app.mylite/tmp/<runtime-id>`,
`--plugin-dir=app.mylite/run/<runtime-id>/plugins`, and
`--aria-log-dir-path=app.mylite/datadir`. InnoDB data, redo, undo, and
temporary paths are also pinned under `datadir/` and per-runtime `tmp/`
children. Startup disables server-owned topology and instrumentation surfaces
with `--skip-grant-tables`,
`--skip-networking`, `--skip-log-bin`, and `--skip-slave-start`; Performance
Schema is omitted by the default build profile or disabled when a custom build
includes it. Clean shutdown removes the current runtime's `run/` and `tmp/`
children, prunes an empty `run/` root, and leaves durable native storage in
`datadir/`. `mylite.lock` remains as a stable lock anchor.

Format 1 uses `mylite.meta` as the directory identity marker:

```text
format=1
mariadb_base=mariadb-11.8.6
```

The ownerless-concurrency foundation adds
`concurrency/mylite-concurrency.meta`:

```text
format=1
mariadb_base=mariadb-11.8.6
database_uuid=<uuid>
concurrency_generation=0
mode=exclusive
```

Opening an existing directory without `mylite.meta` is allowed only when the
directory is empty and the caller passes `MYLITE_OPEN_CREATE`. Non-empty
directories without valid metadata, or directories missing required `datadir/`
or `tmp/` entries, are treated as invalid MyLite database directories rather
than being silently repaired. Stale inactive `run/` and `tmp/` runtime children
are replaced only after the process acquires `mylite.lock`; live runtime
children are process-scoped so future non-exclusive modes do not remove another
process's runtime files.
Existing format-1 directories that predate `mylite-concurrency.meta` are
upgraded by creating that file after the required root metadata and layout have
been validated.

The layout must be validated by tests that open a database, execute DDL and DML,
close it, and assert that durable state did not appear outside the MyLite
database directory.

## Native Engine Policy

Supported MariaDB engines use their own formats inside `datadir/`.

- `InnoDB` may use tablespaces, redo, undo, doublewrite, and recovery state when
  those files stay inside the MyLite database directory.
- `MyISAM` may use `.MYD` and `.MYI` table files inside the MyLite database
  directory.
- `Aria` may use `.MAI`, `.MAD`, `aria_log.*`, and `aria_log_control` inside
  the MyLite database directory.
- Zero-file engines such as `MEMORY` may be supported when their behavior fits
  the embedded profile. Optional dynamic engines such as `BLACKHOLE` require a
  dedicated profile decision before they are exposed.
- Dynamic external engines are out of scope for the default embedded profile
  unless a slice explicitly designs loading, ownership, and directory-boundary
  guarantees.

The default storage engine should be the selected MariaDB build's native
default unless a later slice adds an explicit MyLite configuration policy.
Application `ENGINE=` clauses should be honored for supported native engines
and rejected clearly for unsupported engines.

## Metadata And Schemas

MyLite should use MariaDB's native metadata behavior where practical. Schema and
table metadata may be represented by MariaDB files inside the MyLite database
directory; they are not user-managed sidecars when they stay within that
directory.

`CREATE DATABASE`, `DROP DATABASE`, `USE`, table-name resolution, and
information schema listing should follow MariaDB behavior over the embedded
directory. The important MyLite guarantee is lifecycle containment, not hiding
MariaDB's internal metadata layout.

The first metadata lifecycle coverage validates controlled MyISAM
`CREATE TABLE`, `ALTER TABLE`, `RENAME TABLE`, and `DROP TABLE` behavior by
checking MariaDB `db.opt`, `.frm`, `.MYD`, and `.MYI` files under `datadir/`.
It does not claim broader engine, metadata-object, crash-recovery, or
concurrency behavior.

Native table operation coverage validates controlled MyISAM row DML, scans,
primary and secondary indexes, duplicate-key diagnostics, nullable unique keys,
autoincrement state across reopen, `TEXT` and `BLOB` values, and copy-style
`ALTER TABLE` rebuilds through MariaDB's native handler path. This is still
MyISAM-specific evidence; concurrency, broader InnoDB behavior, and Aria table
behavior require separate slices.

Transaction and recovery coverage validates explicit `ENGINE=InnoDB` tables for
commit, rollback, savepoint rollback, release savepoint, clean reopen, and
child-process recovery. That coverage proves representative native InnoDB
transaction behavior and file containment inside the MyLite database directory;
it does not prove all isolation levels, online DDL, or cross-process writer
safety.

Engine and application-schema coverage validates explicit `ENGINE=InnoDB`,
`ENGINE=MyISAM`, `ENGINE=Aria`, and `ENGINE=MEMORY` table creation, MyISAM
durable row state for durable engines, MEMORY table definitions with empty row
state after reopen, MariaDB default-engine resolution, and representative
WordPress-shaped InnoDB DDL. MyLite follows the selected MariaDB build default;
the current embedded profile resolves no-engine DDL to InnoDB.

Broader DDL coverage validates representative `CREATE TABLE ... LIKE`,
`CREATE TABLE ... SELECT`, standalone index create/drop, default-engine
`ALTER TABLE` column and index changes, CHECK constraint enforcement, InnoDB
foreign-key enforcement with cascade delete, and stored/virtual generated
columns. This is still bounded compatibility evidence; views, triggers, stored
functions, online DDL algorithms, partition DDL, and metadata edge cases need
separate slices before they can be claimed.

The default embedded profile does not expose server account administration,
dynamic plugin or UDF loading, replication metadata, binlog administration, SQL
`HANDLER`, foreign-server metadata, external backup coordination, host-file SQL
import/export, or the event scheduler. Server help-table lookup, statement
profiling, query-cache management, query logs, optimizer trace, process-list
metadata, status counters, user statistics, and user-variable diagnostics are
also outside the core embedded storage profile; inherited network client
auth-plugin negotiation is outside the core embedded API. Direct execution and
prepared-statement preparation reject those top-level SQL command families
before they can create server sidecars, depend on `mysql.*` system tables,
read or write caller-named files, or expose server-owned tuning state.
`information_schema` remains virtual. MyLite initializes the minimal
`mysql.proc` / `mysql.procs_priv` metadata needed by MariaDB stored routines
inside the database directory. Any future required `mysql.*` system surface
should likewise be created or maintained inside the MyLite database directory,
or exposed as a read-only virtual surface when persistent server tables are
unnecessary.

## Transactions, Recovery, And Concurrency

Transaction, recovery, and write-concurrency guarantees come from the selected
MariaDB native storage engine and the embedded configuration MyLite applies.
MyLite must not claim stronger behavior than the configured native engine
provides.

Minimum MyLite responsibilities:

- place engine recovery files inside the MyLite database directory,
- configure temporary and runtime paths deliberately,
- reject or serialize opens that would violate native engine locking rules,
  including cross-process read/write opens while another process owns the
  MyLite directory lock,
- test clean open/close, crash/reopen, and recovery-file cleanup behavior,
- document per-engine limits rather than hiding them behind broad compatibility
  claims.

Exclusive read/write opens remain the default. `MYLITE_OPEN_OWNERLESS_RW` enables
the tested ownerless InnoDB cross-process subset without a daemon or owner
process. Broader DDL invalidation, purge/undo-free coordination, transaction-aware
page-version reads, retained-record checkpoint replay, long-running stress, and
non-InnoDB ownerless engines remain planned work.

## Temporary Data

Temporary tables, query spill files, and runtime files are storage policy, not
violations of the single-directory model.

- User temporary tables start as session-local state and do not need to become
  durable application state.
- Internal temporary spill should use `tmp/` under the MyLite database directory
  by default.
- The shared InnoDB temporary tablespace (`datadir/ibtmp1`) is part of the
  ownerless directory lifecycle while ownerless peers are active; startup and
  shutdown must not delete it unless the current process is the last active
  ownerless runtime.
- Strict no-temp-file modes may exist, but they trade off query limits and
  performance.
- Runtime companions must use deterministic names or subdirectories and must be
  covered by lifecycle tests.

## Migration

MyLite should not initially promise to open arbitrary live MariaDB datadirs as
MyLite database directories. Migration is logical first:

- import SQL dumps,
- export MariaDB-compatible SQL dumps where practical,
- consider stopped-datadir adoption only after directory layout, engine
  configuration, and version compatibility rules are designed and tested.

## Source References

- MariaDB embedded interface: <https://mariadb.com/kb/en/embedded-mariadb-interface/>
- Aria storage and log files: <https://mariadb.com/docs/server/server-usage/storage-engines/aria/aria-storage-engine>
- InnoDB tablespaces: <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/innodb-tablespaces/innodb-file-per-table-tablespaces>
