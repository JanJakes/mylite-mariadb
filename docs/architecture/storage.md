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

The native-storage baseline starts MariaDB with `--datadir=app.mylite/datadir`,
`--tmpdir=app.mylite/tmp`, `--plugin-dir=app.mylite/run/plugins`, and
`--aria-log-dir-path=app.mylite/datadir`. InnoDB data, redo, undo, and
temporary paths are also pinned under `datadir/` and `tmp/`. Startup disables
server-owned topology and instrumentation surfaces with `--skip-grant-tables`,
`--skip-networking`, `--skip-log-bin`, `--skip-slave-start`, and
`--performance-schema=OFF`. Clean shutdown removes `run/` and clears temporary
files under `tmp/`; durable native storage remains in `datadir/`. `mylite.lock`
remains as a stable lock anchor.

Format 1 uses `mylite.meta` as the directory identity marker:

```text
format=1
mariadb_base=mariadb-11.8.6
```

Opening an existing directory without `mylite.meta` is allowed only when the
directory is empty and the caller passes `MYLITE_OPEN_CREATE`. Non-empty
directories without valid metadata, or directories missing required `datadir/`
or `tmp/` entries, are treated as invalid MyLite database directories rather
than being silently repaired. Stale inactive `run/` state is replaced only
after the process acquires `mylite.lock`; live `run/` state is preserved for
additional handles to the same active directory and for other processes that
fail to acquire the lock.

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

The default storage engine should be a MariaDB native engine selected by MyLite
configuration. Application `ENGINE=` clauses should be honored for supported
native engines and rejected clearly for unsupported engines.

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
it does not make InnoDB the default engine, prove all isolation levels, prove
foreign keys or online DDL, or claim cross-process writer safety.

Engine and application-schema coverage validates explicit `ENGINE=InnoDB`,
`ENGINE=MyISAM`, `ENGINE=Aria`, and `ENGINE=MEMORY` table creation, MyISAM
default-engine resolution, durable row state for durable engines, MEMORY table
definitions with empty row state after reopen, and representative
WordPress-shaped InnoDB DDL.

The default embedded profile does not expose server account administration,
dynamic plugin installation, replication metadata, binlog administration, or
the event scheduler. Direct execution and prepared-statement preparation reject
those top-level SQL command families before they can create server sidecars or
depend on `mysql.*` system tables. `information_schema` remains virtual. Any
future required `mysql.*` system surface should be created or maintained inside
the MyLite database directory, or exposed as a read-only virtual surface when
persistent server tables are unnecessary.

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
