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
| Read-only opens | ⚪&nbsp;Planned | Reserved until native storage can enforce read-only engine access |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds; native-storage coverage verifies MyISAM DDL/DML, row/index operations, and explicit InnoDB transaction/recovery behavior across reopen |
| Prepared statements | 🟡&nbsp;Partial | Reusable MariaDB prepared statements are exposed through `mylite_prepare()`, `mylite_step()`, `mylite_reset()`, and `mylite_finalize()` with 1-based parameter binding |
| Binary-safe values | 🟡&nbsp;Partial | Prepared text/blob bindings and column accessors use explicit byte counts; embedded NUL blob values are covered |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text |
| Warnings | 🟡&nbsp;Partial | MariaDB warning counts and indexed warning lookup expose level, code, and message text after statement execution |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct execution exposes affected rows for non-result statements and the last insert id |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| No explicit engine | 🟡&nbsp;Partial | The baseline configures MariaDB's native MyISAM engine as the temporary default; default-engine table creation and persistence are covered while broader engine policy is designed |
| `ENGINE=MYLITE` | ➖&nbsp;Out&nbsp;of&nbsp;scope | No separate MyLite engine in the native-storage directory model |
| `ENGINE=InnoDB` | 🟡&nbsp;Partial | Explicit InnoDB tables use native InnoDB files inside the MyLite database directory; controlled transaction/recovery behavior and WordPress-shaped DDL are covered, while broader InnoDB features remain planned |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | Controlled create/alter/rename/drop/reopen and row/index coverage verifies `.frm`, `.MYD`, and `.MYI` files inside `datadir/` |
| `ENGINE=Aria` | 🟡&nbsp;Partial | Explicit Aria table creation, row persistence, and `.MAI`/`.MAD` files under `datadir/` are covered |
| `ENGINE=MEMORY` | 🟡&nbsp;Partial | Explicit MEMORY table creation is covered; table definitions survive reopen while rows remain process-local and empty after reopen |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Default embedded profile does not load storage-engine plugins; explicit `ENGINE=BLACKHOLE` and `ENGINE=ARCHIVE` requests are covered as rejected |

## Directory Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database directory | 🟡&nbsp;Partial | Open/create establishes and validates a MyLite-owned directory with `mylite.meta`, `mylite.lock`, `datadir/`, `tmp/`, and process-local `run/`; `.mylite/` is recommended but not enforced |
| MariaDB metadata files | 🟡&nbsp;Partial | Controlled schema and MyISAM table metadata lifecycle is covered for `db.opt`, `.frm`, create, alter, rename, and drop paths inside `datadir/` |
| InnoDB files | 🟡&nbsp;Partial | Representative InnoDB tablespace, redo, undo, and temporary files are configured and covered inside the MyLite database directory |
| MyISAM files | 🟡&nbsp;Partial | Controlled lifecycle and native table operation coverage verifies `.MYD` and `.MYI` table files stay inside `datadir/` across create, row DML, copy alter, rename, drop, and reopen |
| Aria files | 🟡&nbsp;Partial | Runtime startup sets `--aria-log-dir-path=<db>/datadir`; explicit Aria table coverage verifies `.MAI` and `.MAD` files under `datadir/` |
| MEMORY definitions | 🟡&nbsp;Partial | Explicit MEMORY table coverage verifies persistent table metadata under `datadir/` and empty row state after reopen |
| MyLite-owned transient paths | 🟡&nbsp;Partial | Durable database paths use `tmp/`, `run/`, and `mylite.lock` inside the database directory; clean close removes `run/` and clears `tmp/`, and clean open replaces stale inactive `run/` state after taking the directory lock |
| Durable files outside the database directory | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-surface policy coverage rejects or disables known server-owned paths that could create replication, binlog, performance-schema, or `mysql.*` sidecars outside the supported application-storage model |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | 🟡&nbsp;Partial | Controlled MyISAM create, drop, and rename lifecycle is covered for native metadata and engine files; explicit InnoDB, Aria, MEMORY, and default-engine create coverage is also present, while broader DDL forms remain planned |
| `ALTER TABLE` | 🟡&nbsp;Partial | Controlled MyISAM `ADD COLUMN` and copy-style `ADD KEY` lifecycle is covered across close and reopen; broader algorithms and engines remain planned |
| Standalone `CREATE INDEX` / `DROP INDEX` | ⚪&nbsp;Planned | Route through MariaDB DDL and native engine index updates |
| `CREATE TABLE ... LIKE` | ⚪&nbsp;Planned | Preserve MariaDB table definition behavior |
| `CREATE TABLE ... SELECT` | ⚪&nbsp;Planned | Preserve MariaDB statement semantics over MyLite tables |
| Schemas/databases | 🟡&nbsp;Partial | Controlled `CREATE DATABASE`, qualified table access, and `DROP DATABASE` lifecycle are covered inside `datadir/`; broader schema behavior remains planned |
| Representative application schemas | 🟡&nbsp;Partial | WordPress-shaped InnoDB `wp_options`, `wp_posts`, and `wp_postmeta` DDL and queries are covered as representative application-schema evidence |
| Views, triggers, and routines | ⚪&nbsp;Planned | Persist through MariaDB native metadata inside the MyLite directory where supported |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile; event DDL and scheduler variables are rejected by policy coverage |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded directory ownership replaces server account management; account, role, grant, revoke, and password statements are rejected by policy coverage |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior; replication and binlog command families are rejected, `@@log_bin=0` is covered, and the default embedded archive omits the active binlog transaction/event core |
| Dynamic plugin installation | ➖&nbsp;Out&nbsp;of&nbsp;scope | The embedded core uses a transient database-local plugin directory and rejects `INSTALL PLUGIN` / `UNINSTALL PLUGIN` through policy coverage |
| Dynamic UDF registration | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-owned shared-library loading; `CREATE FUNCTION ... SONAME` and aggregate UDF registration are rejected by policy and the default embedded archive omits the UDF runtime |
| Oracle SQL mode | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional MariaDB compatibility mode, not core MySQL/MariaDB application behavior; attempts to set `sql_mode=ORACLE` are rejected and the embedded archive links an unsupported parser stub |
| SQL `HELP` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server help-table lookup depends on `mysql.*` help tables and is rejected by policy coverage |
| Statement profiling | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic surface; `@@have_profiling=NO` is covered and profiling commands or variables are rejected by policy coverage |
| Query cache | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-side result-cache optimization; `@@have_query_cache=NO` is covered, management commands and variables are rejected, and `SQL_CACHE` / `SQL_NO_CACHE` remain accepted no-op hints |
| Optimizer trace | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic surface; optimizer-trace variables and `INFORMATION_SCHEMA.OPTIMIZER_TRACE` reads, including unqualified reads while `information_schema` is current, are rejected by policy and omitted from the default embedded archive while ordinary planning, execution, and `EXPLAIN` remain supported |
| General and slow query logs | ➖&nbsp;Out&nbsp;of&nbsp;scope | Daemon query-audit diagnostics; query-log variables and log flush commands are rejected by policy, `@@general_log=0`, `@@slow_query_log=0`, and `@@log_output=NONE` are covered, and the default embedded archive omits query-log handlers while error logging and SQL diagnostics remain available |
| Statement digest diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Performance Schema diagnostic surface; the default embedded archive omits statement digest normalization, `@@max_digest_length=0` is covered, and ordinary parsing, execution, prepared statements, diagnostics, and `EXPLAIN` remain supported |
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
| CHECK constraints | ⚪&nbsp;Planned | Use MariaDB expression evaluation and persist metadata through native metadata paths |
| Foreign keys | ⚪&nbsp;Planned | InnoDB-compatible semantics where practical; reject unsupported cases explicitly |
| Generated columns | ⚪&nbsp;Planned | Preserve MariaDB virtual/stored generated-column behavior through storage support |
| FULLTEXT and SPATIAL indexes | ⚪&nbsp;Planned | Support only where the selected native engine and embedded profile support them |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | 🟡&nbsp;Partial | Explicit InnoDB transactions commit through native MariaDB/InnoDB hooks inside the MyLite database directory |
| Rollback | 🟡&nbsp;Partial | Explicit InnoDB transaction rollback is covered; MyISAM remains non-transactional |
| Savepoints | 🟡&nbsp;Partial | Explicit InnoDB savepoint rollback and release savepoint are covered through SQL transaction statements |
| Crash recovery | 🟡&nbsp;Partial | Parent-process reopen after child-process exit covers committed InnoDB rows surviving and uncommitted rows rolling back |
| Multiple readers | ⚪&nbsp;Planned | Safe readers over stable committed state |
| Concurrent writers | ⚪&nbsp;Planned | Preserve the selected native engine's write-concurrency behavior |
| Cross-process unsafe writers | 🟡&nbsp;Partial | A second read/write process open is rejected with `MYLITE_BUSY` while another process owns the MyLite directory lock |
