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
- `tmp/` is the default location for MyLite-owned temporary files and query
  spill that must not escape the database directory.
- `run/` is for lock files, process-local markers, or runtime state that is
  safe to remove after clean shutdown.
- `concurrency/mylite-concurrency.meta` records the durable identity and
  generation seed for future ownerless coordination. It does not enable
  shared read-only or ownerless read/write opens by itself.
- `concurrency/mylite-concurrency.lock` is the future byte-range lock anchor.
  The current exclusive mode uses its `PERSISTED_CONFIG`, `RECOVERY`,
  `SHM_RESIZE`, and `OPEN_REGISTRY` ranges while creating or validating
  concurrency metadata, shared-memory layout, stale `.shm` rebuilds, and the
  process registry. Shared-memory preparation takes `RECOVERY` before
  `SHM_RESIZE`, matching the planned global ownerless lock order.
- `concurrency/mylite-concurrency.shm` is a grow-only file-backed shared-memory
  file. It starts with a fixed 128-byte MyLite header containing a magic value,
  format markers, byte-order marker, clean/dirty/rebuilding state, mapping
  size, generation counters, segment-table metadata, and the database UUID from
  `mylite-concurrency.meta`. The first segments are a fixed process registry,
  fixed wait-channel table, and fixed MDL lock-table foundation; exclusive
  opens map the file, mark `.shm` dirty, allocate one active process slot for
  the embedded runtime, and release that slot before marking `.shm` clean on
  final close. Clean opens preserve the existing registry, wait-channel, and
  MDL lock-table segments. A later open treats dirty or rebuilding `.shm` state
  as stale volatile coordination state, rebuilds the registry, wait channels,
  and lock-table foundation, and increments the recovery-generation field. The
  file is created at a minimum size for future coordination, is validated through a
  `MAP_SHARED` mapping during durable opens, is never shrunk by open, and stale
  or invalid header bytes are rebuilt because the `.shm` file is not durable
  truth. MyLite has an internal mapped latch wait backend, internal ownerless
  platform probe, internal process-slot allocator, internal shared/exclusive
  lock-table primitive with repeated-owner reference counts and same-owner mode
  upgrades, dead-owner cleanup, stable schema/table MDL key hashing, and an
  internal transaction-registry primitive for cross-process monotonic
  transaction IDs, active-ID snapshots, oldest-active tracking, stale end
  rejection, and dead-owner transaction cleanup. The transaction registry is
  covered as a standalone shared-memory primitive and is not wired into the
  production `.shm` segment or InnoDB read views yet.
  MariaDB's embedded MDL ticket lifecycle now has a MyLite hook surface for
  schema/table lock acquire and release balancing, including cloned tickets,
  upgrades, and downgrades. The production `libmylite` runtime registers that
  hook against the directory-backed MDL lock-table segment using the runtime
  process-slot owner while the current exclusive directory lock is still held.
- `concurrency/mylite-concurrency.wal` and
  `concurrency/mylite-concurrency.ckpt` are durable coordination-log and
  checkpoint anchors for future ownerless recovery. They currently contain
  fixed headers with magic, format, byte-order marker, generation, and the
  database UUID, but no recovery records yet.

The native-storage baseline starts MariaDB with `--datadir=app.mylite/datadir`,
`--tmpdir=app.mylite/tmp`, `--plugin-dir=app.mylite/run/plugins`, and
`--aria-log-dir-path=app.mylite/datadir`. InnoDB data, redo, undo, and
temporary paths are also pinned under `datadir/` and `tmp/`. Startup disables
server-owned topology and instrumentation surfaces with `--skip-grant-tables`,
`--skip-networking`, `--skip-log-bin`, and `--skip-slave-start`; Performance
Schema is omitted by the default build profile or disabled when a custom build
includes it. Clean shutdown removes `run/` and clears temporary files under
`tmp/`; durable native storage remains in `datadir/`. `mylite.lock` remains as
a stable lock anchor.

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
than being silently repaired. Stale inactive `run/` state is replaced only
after the process acquires `mylite.lock`; live `run/` state is preserved for
additional handles to the same active directory and for other processes that
fail to acquire the lock.
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

Cross-process read/write ownership is exclusive. Multiple readers and
concurrent writers require explicit tests before support is claimed.

The planned ownerless cross-process concurrency direction is documented in
[Ownerless Cross-Process Concurrency](../specs/ownerless-cross-process-concurrency/specs.md).
That design keeps the current no-daemon product shape and proposes a
directory-backed `mmap(MAP_SHARED)` coordination file under
`concurrency/mylite-concurrency.shm`, backed by byte-range locks and durable
logs. Until that plan is implemented and tested, the supported behavior remains
the exclusive cross-process read/write ownership described above.

## Temporary Data

Temporary tables, query spill files, and runtime files are storage policy, not
violations of the single-directory model.

- User temporary tables start as session-local state and do not need to become
  durable application state.
- Internal temporary spill should use `tmp/` under the MyLite database directory
  by default.
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
