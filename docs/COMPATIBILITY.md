# Compatibility

MyLite compatibility is tracked by surface area, not by broad claims. MariaDB
11.8 is the primary behavior authority; MySQL behavior is additional evidence
for drop-in application expectations.

## Status Key

- ✅&nbsp;Covered: implemented and covered by committed tests.
- 🟡&nbsp;Partial: implemented with documented limits and committed tests.
- ⚪&nbsp;Planned: target behavior for an upcoming slice.
- ➖&nbsp;Out&nbsp;of&nbsp;scope: deliberately omitted from the embedded
  single-directory product.

## Baseline

| Area | Target |
| --- | --- |
| MariaDB base | MariaDB 11.8 LTS, initial import ref `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7` |
| Runtime shape | Embedded in-process library, no daemon required for core use |
| Durable storage | One MyLite-owned database directory containing MariaDB native storage files and documented MyLite lifecycle metadata |
| Primary API | `libmylite` directory-owned C API |
| MariaDB C API | Optional adapter, not the primary lifetime model |

## Harness

Compatibility coverage is grouped with CTest labels under the `embedded-dev`
preset:

| Group | Command |
| --- | --- |
| Embedded lifecycle | `ctest --preset embedded-dev -L compat.lifecycle` |
| Directory-boundary detection | `ctest --preset embedded-dev -L compat.directory-boundary` |
| MariaDB-reference SQL results | `ctest --preset embedded-dev -L compat.mariadb-comparison` |
| Crash/reopen behavior | `ctest --preset embedded-dev -L compat.crash-reopen` |
| Concurrency | `ctest --preset embedded-dev -L compat.concurrency` |
| Ownerless primitives | `ctest --preset embedded-dev -L compat.ownerless-primitives` |
| Ownerless transaction hooks | `ctest --preset embedded-dev -L compat.ownerless-transaction` |
| Ownerless InnoDB lock hooks | `ctest --preset embedded-dev -L compat.ownerless-innodb-lock` |
| Ownerless negative proof | `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof` |
| Platform probes | `ctest --preset embedded-dev -L compat.platform` |
| Application queries | `ctest --preset embedded-dev -L compat.application-query` |
| Engine clauses | `ctest --preset embedded-dev -L compat.engine` |
| Server surfaces | `ctest --preset embedded-dev -L compat.server-surface` |
| Current SQL query surface | `ctest --preset embedded-dev -L compat.query` |

The MariaDB-reference group uses expected result vectors pinned to MariaDB 11.8
behavior. It does not require a daemon in the default test path.

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database directory | 🟡&nbsp;Partial | Implemented for read/write local directory paths with one active database directory per process, a `.mylite/` naming convention, validated format-1 metadata, an advisory directory lock, and a native-storage baseline layout under the database directory |
| Capability reporting | 🟡&nbsp;Partial | `mylite_capabilities()` reports available concurrency modes; the embedded backend currently exposes same-process multi-handle support and deliberately leaves shared read-only and ownerless read/write unset until their storage lifecycle, native lock integration, page visibility, and recovery are implemented |
| Read-only opens | ⚪&nbsp;Planned | Reserved until native storage can enforce read-only engine access |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds; native-storage coverage verifies MyISAM DDL/DML, row/index operations, and explicit InnoDB transaction/recovery behavior across reopen |
| Prepared statements | 🟡&nbsp;Partial | Reusable MariaDB prepared statements are exposed through `mylite_prepare()`, `mylite_step()`, `mylite_reset()`, and `mylite_finalize()` with 1-based parameter binding |
| Binary-safe values | 🟡&nbsp;Partial | Prepared text/blob bindings and column accessors use explicit byte counts; embedded NUL blob values are covered |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text; the default embedded profile keeps common MariaDB messages but may use compact generic text for uncommon inherited server errors |
| Warnings | 🟡&nbsp;Partial | MariaDB warning counts and indexed warning lookup expose level, code, and message text after statement execution |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct execution exposes affected rows for non-result statements and the last insert id |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| No explicit engine | 🟡&nbsp;Partial | MyLite follows MariaDB's compiled default storage engine; the current embedded profile resolves to InnoDB and covers no-engine table creation, metadata, persistence, and `@@default_storage_engine` |
| `ENGINE=MYLITE` | ➖&nbsp;Out&nbsp;of&nbsp;scope | No separate MyLite engine in the native-storage directory model |
| `ENGINE=InnoDB` | 🟡&nbsp;Partial | Explicit InnoDB tables use native InnoDB files inside the MyLite database directory; controlled transaction/recovery behavior and WordPress-shaped DDL are covered, while broader InnoDB features remain planned |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | Controlled create/alter/rename/drop/reopen and row/index coverage verifies `.frm`, `.MYD`, and `.MYI` files inside `datadir/` |
| `ENGINE=Aria` | 🟡&nbsp;Partial | Explicit Aria table creation, row persistence, and `.MAI`/`.MAD` files under `datadir/` are covered |
| `ENGINE=MEMORY` | 🟡&nbsp;Partial | Explicit MEMORY table creation is covered; table definitions survive reopen while rows remain process-local and empty after reopen |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Default embedded profile does not load storage-engine plugins; explicit `ENGINE=BLACKHOLE` and `ENGINE=ARCHIVE` requests are covered as rejected |

## Directory Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database directory | 🟡&nbsp;Partial | Open/create establishes and validates a MyLite-owned directory with `mylite.meta`, `mylite.lock`, `datadir/`, `tmp/`, process-local `run/`, and `concurrency/` metadata, lock, shared-memory, WAL, and checkpoint anchors; `.mylite/` is recommended but not enforced |
| Ownerless concurrency metadata | 🟡&nbsp;Partial | The directory owns a durable concurrency metadata file with format, MariaDB base, database UUID, concurrency generation, and exclusive-mode state, protected by a byte-range `PERSISTED_CONFIG` lock while it is created or validated; ownerless read/write remains disabled |
| Ownerless shared-memory file | 🟡&nbsp;Partial | `concurrency/mylite-concurrency.shm` is created and grown under `RECOVERY` then `SHM_RESIZE` byte-range locks, validated through `MAP_SHARED`, starts with a fixed 128-byte MyLite header bound to the database UUID, and contains fixed process-registry, wait-channel, MDL lock-table, transaction-registry, read-view-registry, InnoDB lock-registry, and redo-visibility foundation segments; clean opens preserve those segments, while dirty, rebuilding, or invalid volatile state is rebuilt with an incremented recovery generation; no page-version path uses those segments yet |
| Ownerless recovery anchors | 🟡&nbsp;Partial | `concurrency/mylite-concurrency.wal` and `concurrency/mylite-concurrency.ckpt` are created under `RECOVERY` with fixed headers bound to the database UUID; no durable coordination records are written yet |
| Ownerless coordination primitives | 🟡&nbsp;Partial | POSIX file-backed `MAP_SHARED` visibility, grow/remap behavior, byte-range lock conflicts, lock release on process exit, internal mapped latch wait/wake plus timeout behavior, internal cross-process process-slot allocation, heartbeat update, stale-slot cleanup including exited-process cleanup, dead-owner lock cleanup, an internal cross-process shared/exclusive lock-table primitive with repeated-owner reference counts and same-owner mode upgrades, stable ownerless MDL schema/table key hashing, an internal cross-process transaction registry primitive for monotonic transaction IDs, active-ID snapshots, oldest-active tracking, stale end rejection, and dead-owner transaction cleanup, an internal read-view registry for purge-visible read-view publication and cleanup, and an internal InnoDB table/record lock-registry primitive with MariaDB-compatible table/gap/insert-intention conflict coverage, shared wait-edge publication, wait cleanup, and cross-process wait-cycle detection are covered as platform evidence; product opens now allocate and release a directory process slot, validate the transaction, read-view, and InnoDB lock registry segments, and clean dead-owner transaction/read-view/InnoDB-lock entries while the current exclusive directory lock is still held |
| Ownerless MDL hook surface | 🟡&nbsp;Partial | MariaDB's embedded MDL ticket lifecycle has a MyLite hook point for schema/table metadata-lock acquire and release, including cloned tickets, upgrades, downgrades, and release balancing; `libmylite` registers it against the directory-backed MDL lock-table segment using the runtime process-slot owner while the current exclusive directory lock is still held, so product ownerless read/write remains disabled |
| Ownerless InnoDB transaction and read-view hook surface | 🟡&nbsp;Partial | InnoDB maximum transaction ID reads, transaction ID allocation, read-write transaction registration, transaction serialisation-number assignment, active transaction snapshots, deregistration, read-view publication/removal, and purge oldest-view snapshotting have guarded MyLite hook surfaces covered by embedded InnoDB SQL tests; product opens install those hooks against directory-backed shared state while the current exclusive directory lock is still held, and ownerless read/write remains disabled until page visibility, DDL dictionary invalidation, and recovery are completed and tested together |
| Ownerless InnoDB lock hook surface | 🟡&nbsp;Partial | InnoDB table-lock creation/removal, record-lock bitmap bit set/reset, waiting-lock grant, record-lock object dequeue, wait enqueue/reset, and discard paths have guarded MyLite hook coverage that mirrors granted native locks and local wait edges into the directory-backed InnoDB lock-registry segment; locks acquired before `trx_t::id` exists use a stable transient MyLite lock identity until release; embedded SQL tests verify lock entries appear during real InnoDB write transactions and DDL locking-read paths, local row-lock waits publish and clear shared wait entries, granted locks release on commit after dirty pages are flushed through the transaction commit LSN, rollback, close, and dead-owner cleanup, pre-grant reservation prevents a local grant when the shared registry already contains a conflicting external record lock, a cross-process external record conflict waits and wakes after release, cross-process deadlocks return MariaDB errno 1213, shared-registry timeout maps to MariaDB errno 1205, and post-wait redo visibility is refreshed before InnoDB retries the local grant. Product ownerless read/write remains gated until page visibility is hardened, DDL dictionary invalidation is coordinated, and crash recovery is completed |
| MariaDB metadata files | 🟡&nbsp;Partial | Controlled schema and MyISAM table metadata lifecycle is covered for `db.opt`, `.frm`, create, alter, rename, and drop paths inside `datadir/` |
| InnoDB files | 🟡&nbsp;Partial | Representative InnoDB tablespace, redo, undo, and temporary files are configured and covered inside the MyLite database directory |
| MyISAM files | 🟡&nbsp;Partial | Controlled lifecycle and native table operation coverage verifies `.MYD` and `.MYI` table files stay inside `datadir/` across create, row DML, copy alter, rename, drop, and reopen |
| Aria files | 🟡&nbsp;Partial | Runtime startup sets `--aria-log-dir-path=<db>/datadir`; explicit Aria table coverage verifies `.MAI` and `.MAD` files under `datadir/` |
| MEMORY definitions | 🟡&nbsp;Partial | Explicit MEMORY table coverage verifies persistent table metadata under `datadir/` and empty row state after reopen |
| MyLite-owned transient paths | 🟡&nbsp;Partial | Durable database paths use per-runtime `tmp/<runtime-id>/`, `run/<runtime-id>/`, and `mylite.lock` inside the database directory; clean close removes the current runtime's children and prunes an empty `run/` root, clean exclusive open replaces stale inactive runtime children after taking the directory lock, and `:memory:` uses a transient runtime directory that is removed on final close |
| Durable files outside the database directory | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-surface policy coverage rejects or disables known server-owned paths that could create replication, binlog, performance-schema, or `mysql.*` sidecars outside the supported application-storage model |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | 🟡&nbsp;Partial | Controlled MyISAM create, drop, and rename lifecycle is covered for native metadata and engine files; explicit InnoDB, Aria, MEMORY, MariaDB default-engine create, `CREATE TABLE ... LIKE`, and `CREATE TABLE ... SELECT` coverage is also present |
| `ALTER TABLE` | 🟡&nbsp;Partial | Controlled MyISAM `ADD COLUMN` and copy-style `ADD KEY` lifecycle is covered across close and reopen; representative default-engine InnoDB column modify/change and index add/drop changes are covered, while online algorithms and broader edge cases remain planned |
| Standalone `CREATE INDEX` / `DROP INDEX` | 🟡&nbsp;Partial | Representative default-engine InnoDB standalone index create/drop is covered through MariaDB DDL and native engine metadata |
| `CREATE TABLE ... LIKE` | 🟡&nbsp;Partial | Representative MariaDB table-definition copy behavior is covered for default-engine tables |
| `CREATE TABLE ... SELECT` | 🟡&nbsp;Partial | Representative CTAS behavior is covered over MyLite tables |
| Schemas/databases | 🟡&nbsp;Partial | Controlled `CREATE DATABASE`, qualified table access, and `DROP DATABASE` lifecycle are covered inside `datadir/`; broader schema behavior remains planned |
| Sequences | 🟡&nbsp;Partial | Simple MariaDB `CREATE SEQUENCE ... NOCACHE` and `NEXT VALUE FOR` behavior is covered across close and reopen; broader sequence DDL and edge cases remain planned |
| Representative application schemas | 🟡&nbsp;Partial | WordPress-shaped InnoDB `wp_options`, `wp_posts`, and `wp_postmeta` DDL and queries are covered as representative application-schema evidence |
| Views, triggers, and routines | 🟡&nbsp;Partial | Minimal `mysql.proc` / `mysql.procs_priv` metadata is initialized inside the MyLite directory and simple result-returning direct stored-procedure create, show, call, and drop behavior is covered; prepared `CALL`, broader views, triggers, stored functions, routine edge cases, empty-result metadata, and metadata compatibility remain planned |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile; event DDL, event metadata commands, and scheduler variables are rejected by policy coverage, and the default embedded archive uses only a parser-link event parse-data stub |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded directory ownership replaces server account management; account, role, grant, revoke, and password statements are rejected by policy coverage, and the default embedded archive omits the `unix_socket` server auth plugin |
| Foreign-server metadata | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-global remote connection metadata; `CREATE SERVER`, `CREATE OR REPLACE SERVER`, `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` are rejected by policy coverage, and the default embedded archive omits the `mysql.servers` metadata cache |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior; replication and binlog command families, SQL `BINLOG` replay, GTID helper functions, GTID state variable assignments, and binary-log GTID-index tuning variables are rejected or omitted, `@@log_bin=0` is covered, and the default embedded archive omits the active binlog transaction/event core, SQL `BINLOG` replay source, server event writers, binary-log event parser/reader runtime, replication GTID-state runtime, binary-log GTID-index runtime, residual replication helper objects, unsupported injector root, guarded replication execution system variables, and replication/binlog filter runtime |
| External XA transactions | ➖&nbsp;Out&nbsp;of&nbsp;scope | Distributed transaction-manager surface; direct and prepared `XA` statements are rejected by policy coverage, and the default embedded archive omits the external-XA runtime plus the mmap-backed `tc.log` transaction coordinator while ordinary native-engine transactions remain covered |
| SQL `HANDLER` commands | ➖&nbsp;Out&nbsp;of&nbsp;scope | Low-level server table-cursor surface; direct and prepared top-level `HANDLER ...` statements are rejected by policy coverage, and the default embedded archive omits SQL `HANDLER` command runtime while retaining MariaDB's storage-engine `handler` abstraction |
| Host-file SQL exports | ➖&nbsp;Out&nbsp;of&nbsp;scope | `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` write arbitrary host files outside result delivery; direct and prepared forms are rejected by policy coverage, and the default embedded archive omits the host-file writer bodies while retaining `SELECT ... INTO` variables |
| Host-file SQL imports | ➖&nbsp;Out&nbsp;of&nbsp;scope | `LOAD DATA` and `LOAD XML` read arbitrary host files or client-protocol file streams outside the `libmylite` parameter API; direct and prepared forms are rejected by policy coverage, and the default embedded archive omits the import runtime while retaining ordinary `INSERT`, prepared bindings, and `INSERT ... SELECT` |
| Dynamic plugin installation | ➖&nbsp;Out&nbsp;of&nbsp;scope | The embedded core uses a transient database-local plugin directory, rejects `INSTALL PLUGIN` / `UNINSTALL PLUGIN` through policy coverage, reports `@@have_dynamic_loading=NO`, and omits runtime shared-object plugin loading from the default archive |
| Dynamic UDF registration | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-owned shared-library loading; `CREATE FUNCTION ... SONAME` and aggregate UDF registration are rejected by policy and the default embedded archive omits the UDF runtime |
| VIO TLS transport | ➖&nbsp;Out&nbsp;of&nbsp;scope | Core `libmylite` opens a local database directory without a socket or TLS handshake; the default embedded archive omits MariaDB's VIO TLS transport, inherited `mysql_ssl_set()` calls fail closed, and first-party linked artifacts no longer depend on `libssl`, while SQL crypto functions retain `libcrypto` |
| Network client authentication handshake | ➖&nbsp;Out&nbsp;of&nbsp;scope | Core `libmylite` opens a local database directory without client/server auth-plugin negotiation; the default embedded archive omits inherited client auth plugin descriptors and plugin VIO handshake helpers, while raw remote client auth and `mysql_change_user()` fail closed |
| PROXY protocol listener | ➖&nbsp;Out&nbsp;of&nbsp;scope | Core `libmylite` has no socket listener or network handshake; the default embedded archive omits MariaDB's PROXY protocol parser and `proxy_protocol_networks` system variable |
| Oracle SQL mode | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional MariaDB compatibility mode, not core MySQL/MariaDB application behavior; attempts to set `sql_mode=ORACLE` are rejected and the embedded archive links an unsupported parser stub |
| Oracle compatibility function aliases | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional Oracle migration aliases such as `DECODE_ORACLE`, `LPAD_ORACLE`, and `oracle_schema` routing are omitted from the default embedded archive; ordinary MySQL/MariaDB string functions remain covered |
| SQL `HELP` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server help-table lookup depends on `mysql.*` help tables and is rejected by policy coverage |
| Statement profiling | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic surface; `@@have_profiling=NO` is covered, profiling commands, variables, and `INFORMATION_SCHEMA.PROFILING` reads are rejected by policy coverage, and the default embedded archive omits the remaining profiling metadata source |
| Query cache | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-side result-cache optimization; `@@have_query_cache=NO` is covered, management commands and variables are rejected, and `SQL_CACHE` / `SQL_NO_CACHE` remain accepted no-op hints |
| Optimizer trace | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic surface; optimizer-trace variables and `INFORMATION_SCHEMA.OPTIMIZER_TRACE` reads, including unqualified reads while `information_schema` is current, are rejected by policy and omitted from the default embedded archive while ordinary planning, execution, and `EXPLAIN` remain supported |
| Persistent optimizer statistics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-owned `mysql.*` optimizer-statistics metadata; the default embedded profile starts with `@@use_stat_tables=NEVER` and `@@histogram_size=0`, rejects persistent `ANALYZE TABLE ... PERSISTENT FOR ...` and statistic variable changes, and omits persistent statistics plus JSON histogram storage while ordinary `ANALYZE TABLE`, engine estimates, planning, and `EXPLAIN` remain supported |
| General and slow query logs | ➖&nbsp;Out&nbsp;of&nbsp;scope | Daemon query-audit diagnostics; query-log variables and log flush commands are rejected by policy, `@@general_log=0`, `@@slow_query_log=0`, and `@@log_output=NONE` are covered, and the default embedded archive omits query-log handlers while error logging and SQL diagnostics remain available |
| Statement digest diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Performance Schema diagnostic surface; the default embedded archive omits statement digest normalization, `@@max_digest_length=0` is covered, and ordinary parsing, execution, prepared statements, diagnostics, and `EXPLAIN` remain supported |
| Server status variables | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic counters; the default embedded archive omits status-variable publication, `SHOW STATUS` and status Information Schema tables return empty rows, and ordinary SQL diagnostics, warnings, result metadata, and the public API remain available |
| Process-list metadata | ➖&nbsp;Out&nbsp;of&nbsp;scope | Daemon thread/session inventory; `SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` are rejected, `INFORMATION_SCHEMA.PROCESSLIST` returns zero rows, and the default embedded archive omits the process-list row producers |
| User statistics diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional server diagnostic counters; the default embedded archive omits the `userstat` plugin and system variable, rejects userstat Information Schema tables and `FLUSH *_STATISTICS`, and keeps ordinary application tables with the same names usable outside `information_schema` |
| User-variable diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional session introspection/reset surface; the default embedded archive omits the `user_variables` plugin, rejects `INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW USER_VARIABLES`, and `FLUSH USER_VARIABLES`, and keeps ordinary `@variable` SQL plus application tables named `user_variables` usable |
| External backup runtime | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server backup-tool coordination surface; `BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` are rejected by policy coverage, and the default embedded archive omits the active backup runtime while keeping ordinary DDL hooks inert |
| Server utility SQL functions | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server benchmarking, named locks, host-file reads, replication waits, sleeping, and server-id based ID generation; direct and prepared `BENCHMARK()`, `GET_LOCK()` and related helpers, `LOAD_FILE()`, replication wait/position helpers, `SLEEP()`, and `UUID_SHORT()` are rejected by policy and omitted from the default embedded archive, while ordinary scalar functions, JSON, GEOMETRY/GIS, DDL/DML, transactions, and native storage remain supported |
| Vector SQL runtime | ➖&nbsp;Out&nbsp;of&nbsp;scope | MariaDB vector conversion, distance, and MHNSW vector-index runtime are not part of the current embedded profile; direct and prepared `VEC_FROMTEXT()`, `VEC_TOTEXT()`, `VEC_DISTANCE()`, `VEC_DISTANCE_EUCLIDEAN()`, and `VEC_DISTANCE_COSINE()` calls are rejected by policy, vector-index DDL is covered as rejected, and the default embedded archive omits `item_vectorfunc.cc`, `vector_mhnsw.cc`, and mandatory `mhnsw` plugin registration while retaining `VECTOR(N)` type parsing as a separate compatibility decision |
| XML SQL helpers | ➖&nbsp;Out&nbsp;of&nbsp;scope | Legacy XPath helper functions; direct and prepared `EXTRACTVALUE()` and `UPDATEXML()` calls are rejected by policy and omitted from the default embedded archive, while ordinary SQL, JSON, GEOMETRY/GIS, native storage, and the separately unsupported `LOAD XML` host-file import boundary remain unchanged |
| Dynamic columns | ➖&nbsp;Out&nbsp;of&nbsp;scope | MariaDB-specific dynamic-column SQL helpers; direct and prepared `COLUMN_CREATE()`, `COLUMN_ADD()`, `COLUMN_DELETE()`, `COLUMN_GET()`, `COLUMN_CHECK()`, `COLUMN_EXISTS()`, `COLUMN_LIST()`, and `COLUMN_JSON()` calls are rejected by policy and the default embedded archive keeps only fail-closed dynamic-column C helper stubs, while ordinary SQL, JSON, GEOMETRY/GIS, native storage, and result metadata remain unchanged |
| `SFORMAT()` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional MariaDB fmtlib-backed formatting helper; omitted from the embedded profile so the embedded SQL target can build without C++ exceptions, while ordinary `FORMAT()` remains available |
| `PROCEDURE ANALYSE()` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Legacy diagnostic SELECT extension; rejected by policy and omitted from the default embedded archive while ordinary SELECT queries remain supported |
| System-variable help comments | 🟡&nbsp;Partial | `SHOW VARIABLES` and system-variable rows, values, defaults, and validation remain available; `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the default embedded profile to omit server help text |
| Static `SHOW` information | ➖&nbsp;Out&nbsp;of&nbsp;scope | `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` expose static server attribution and privilege-help metadata; they are rejected by policy and omitted from the default embedded archive while ordinary supported `SHOW` surfaces remain available |

## Rows, Indexes, And Constraints

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Fixed and variable row fields | 🟡&nbsp;Partial | Controlled MyISAM rows cover integer, variable string, `TEXT`, and `BLOB` storage across update, delete, close, and reopen |
| NULL columns | 🟡&nbsp;Partial | Controlled MyISAM nullable unique-key values are covered; broader NULL comparison and type matrix coverage remains planned |
| BLOB/TEXT values | 🟡&nbsp;Partial | Controlled MyISAM `TEXT` and `BLOB` values are stored, updated, and read through SQL expressions; prepared blob coverage verifies binary-safe values with embedded NUL bytes |
| Primary and secondary indexes | 🟡&nbsp;Partial | Controlled MyISAM primary and secondary indexed predicates are covered, including an index added by copy-style `ALTER TABLE` |
| Unique indexes | 🟡&nbsp;Partial | Controlled MyISAM duplicate-key diagnostics and nullable unique-key inserts are covered |
| Autoincrement | 🟡&nbsp;Partial | Controlled MyISAM table-local autoincrement state is covered across close and reopen |
| CHECK constraints | 🟡&nbsp;Partial | Representative default-engine CHECK constraint metadata and enforcement are covered through MariaDB expression evaluation |
| Foreign keys | 🟡&nbsp;Partial | Representative InnoDB foreign-key enforcement and cascade delete behavior are covered; broader referential edge cases remain planned |
| Generated columns | 🟡&nbsp;Partial | Representative stored and virtual generated-column behavior is covered through native storage support |
| FULLTEXT, SPATIAL, and vector indexes | ⚪&nbsp;Planned | Support only where the selected native engine and embedded profile support them |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | 🟡&nbsp;Partial | Explicit InnoDB transactions commit through native MariaDB/InnoDB hooks inside the MyLite database directory |
| Rollback | 🟡&nbsp;Partial | Explicit InnoDB transaction rollback is covered; MyISAM remains non-transactional |
| Savepoints | 🟡&nbsp;Partial | Explicit InnoDB savepoint rollback and release savepoint are covered through SQL transaction statements |
| Crash recovery | 🟡&nbsp;Partial | Parent-process reopen after child-process exit covers committed InnoDB rows surviving and uncommitted rows rolling back |
| Same-process multi-handle concurrency | 🟡&nbsp;Partial | Multiple `mylite_db` handles over one embedded runtime are covered for committed visibility, simultaneous active InnoDB transactions on different rows, row-lock timeout behavior, shared wait-edge publication for local InnoDB row waits, metadata-lock timeout behavior, savepoints, and foreign-key enforcement |
| Multiple readers | ⚪&nbsp;Planned | Safe readers over stable committed state; shared read-only opens are the first planned step |
| Concurrent writers | ⚪&nbsp;Planned | Ownerless cross-process read/write concurrency requires directory-backed shared-memory coordination, native storage lock integration, page visibility, and crash recovery before support can be claimed |
| Cross-process unsafe writers | 🟡&nbsp;Partial | A second read/write process open is rejected with `MYLITE_BUSY` while another process owns the MyLite directory lock |
| Test-only directory-lock bypass | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset can bypass `mylite.lock` only for negative-proof tests; a second process over the same directory must fail or hang within the bounded proof test, and the hook is unavailable in normal builds |
