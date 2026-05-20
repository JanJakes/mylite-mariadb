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
| Current SQL query surface | `ctest --preset embedded-dev -L compat.query` |

The MariaDB-reference group uses expected result vectors pinned to MariaDB 11.8
behavior. It does not require a daemon in the default test path.

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database directory | 🟡&nbsp;Partial | Implemented for read/write local directory paths with one active database directory per process, a `.mylite/` naming convention, validated format-1 metadata, an advisory directory lock, and a native-storage baseline layout under the database directory |
| Read-only opens | ⚪&nbsp;Planned | Reserved until native storage can enforce read-only engine access |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds; native-storage coverage verifies MyISAM DDL/DML, row/index operations, and explicit InnoDB transaction/recovery behavior across reopen |
| Prepared statements | ⚪&nbsp;Planned | Reusable statements with 1-based parameter binding |
| Binary-safe values | ⚪&nbsp;Planned | Explicit BLOB/TEXT byte counts; no NUL-terminated-value assumptions |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text |
| Warnings | ⚪&nbsp;Planned | MariaDB-compatible warning counts and structured warning access |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct execution exposes affected rows for non-result statements and the last insert id |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| No explicit engine | 🟡&nbsp;Partial | The baseline configures MariaDB's native MyISAM engine as the temporary default while broader engine policy is designed |
| `ENGINE=MYLITE` | ➖&nbsp;Out&nbsp;of&nbsp;scope | No separate MyLite engine in the native-storage directory model |
| `ENGINE=InnoDB` | 🟡&nbsp;Partial | Explicit InnoDB tables use native InnoDB files inside the MyLite database directory; controlled transaction and recovery behavior is covered, while broader InnoDB features remain planned |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | Controlled create/alter/rename/drop/reopen and row/index coverage verifies `.frm`, `.MYD`, and `.MYI` files inside `datadir/` |
| `ENGINE=Aria` | ⚪&nbsp;Planned | Use native Aria files and logs inside the MyLite database directory where supported |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Default embedded profile does not load storage-engine plugins |

## Directory Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database directory | 🟡&nbsp;Partial | Open/create establishes and validates a MyLite-owned directory with `mylite.meta`, `mylite.lock`, `datadir/`, `tmp/`, and process-local `run/`; `.mylite/` is recommended but not enforced |
| MariaDB metadata files | 🟡&nbsp;Partial | Controlled schema and MyISAM table metadata lifecycle is covered for `db.opt`, `.frm`, create, alter, rename, and drop paths inside `datadir/` |
| InnoDB files | 🟡&nbsp;Partial | Representative InnoDB tablespace, redo, undo, and temporary files are configured and covered inside the MyLite database directory |
| MyISAM files | 🟡&nbsp;Partial | Controlled lifecycle and native table operation coverage verifies `.MYD` and `.MYI` table files stay inside `datadir/` across create, row DML, copy alter, rename, drop, and reopen |
| Aria files | 🟡&nbsp;Partial | Runtime startup sets `--aria-log-dir-path=<db>/datadir`; explicit Aria table coverage remains planned |
| MyLite-owned transient paths | 🟡&nbsp;Partial | Durable database paths use `tmp/`, `run/`, and `mylite.lock` inside the database directory; clean close removes `run/` and clears `tmp/`, and clean open replaces stale inactive `run/` state after taking the directory lock |
| Durable files outside the database directory | ➖&nbsp;Out&nbsp;of&nbsp;scope | Reject or reconfigure surfaces that would write durable state outside the MyLite directory |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | 🟡&nbsp;Partial | Controlled MyISAM create, drop, and rename lifecycle is covered for native metadata and engine files; broader DDL forms remain planned |
| `ALTER TABLE` | 🟡&nbsp;Partial | Controlled MyISAM `ADD COLUMN` and copy-style `ADD KEY` lifecycle is covered across close and reopen; broader algorithms and engines remain planned |
| Standalone `CREATE INDEX` / `DROP INDEX` | ⚪&nbsp;Planned | Route through MariaDB DDL and native engine index updates |
| `CREATE TABLE ... LIKE` | ⚪&nbsp;Planned | Preserve MariaDB table definition behavior |
| `CREATE TABLE ... SELECT` | ⚪&nbsp;Planned | Preserve MariaDB statement semantics over MyLite tables |
| Schemas/databases | 🟡&nbsp;Partial | Controlled `CREATE DATABASE`, qualified table access, and `DROP DATABASE` lifecycle are covered inside `datadir/`; broader schema behavior remains planned |
| Views, triggers, and routines | ⚪&nbsp;Planned | Persist through MariaDB native metadata inside the MyLite directory where supported |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded directory ownership replaces server account management |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior |

## Rows, Indexes, And Constraints

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Fixed and variable row fields | 🟡&nbsp;Partial | Controlled MyISAM rows cover integer, variable string, `TEXT`, and `BLOB` storage across update, delete, close, and reopen |
| NULL columns | 🟡&nbsp;Partial | Controlled MyISAM nullable unique-key values are covered; broader NULL comparison and type matrix coverage remains planned |
| BLOB/TEXT values | 🟡&nbsp;Partial | Controlled MyISAM `TEXT` and `BLOB` values are stored, updated, and read through SQL expressions; binary-safe API values remain planned |
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
