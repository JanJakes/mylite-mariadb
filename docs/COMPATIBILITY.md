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

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database directory | 🟡&nbsp;Partial | Implemented for read/write local directory paths with one active database directory per process, a `.mylite/` naming convention, and a native-storage baseline layout under the database directory |
| Read-only opens | ⚪&nbsp;Planned | Reserved until native storage can enforce read-only engine access |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds; native-storage smoke coverage verifies MyISAM DDL/DML persistence across reopen |
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
| `ENGINE=InnoDB` | ⚪&nbsp;Planned | Use native InnoDB files inside the MyLite database directory where supported |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | Controlled create/insert/select/reopen smoke coverage verifies `.MYD` and `.MYI` files inside `datadir/` |
| `ENGINE=Aria` | ⚪&nbsp;Planned | Use native Aria files and logs inside the MyLite database directory where supported |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Default embedded profile does not load storage-engine plugins |

## Directory Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database directory | 🟡&nbsp;Partial | Open/create establishes a MyLite-owned directory with `mylite.meta`, `datadir/`, `tmp/`, and clean-runtime `run/`; `.mylite/` is recommended but not enforced |
| MariaDB metadata files | 🟡&nbsp;Partial | Controlled schema and table metadata are created inside `datadir/`; broader DDL lifecycle is still planned |
| InnoDB files | ⚪&nbsp;Planned | Tablespaces, redo, undo, and recovery files stay inside the MyLite database directory |
| MyISAM files | 🟡&nbsp;Partial | Smoke coverage verifies `.MYD` and `.MYI` table files stay inside `datadir/` across reopen |
| Aria files | 🟡&nbsp;Partial | Runtime startup sets `--aria-log-dir-path=<db>/datadir`; explicit Aria table coverage remains planned |
| MyLite-owned transient paths | 🟡&nbsp;Partial | Durable database paths use `tmp/` and `run/` inside the database directory; clean close removes `run/` and clears `tmp/` |
| Durable files outside the database directory | ➖&nbsp;Out&nbsp;of&nbsp;scope | Reject or reconfigure surfaces that would write durable state outside the MyLite directory |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | 🟡&nbsp;Partial | Controlled MyISAM `CREATE TABLE` is covered; drop, rename, alter, and broader metadata lifecycle remain planned |
| `ALTER TABLE` | ⚪&nbsp;Planned | Preserve MariaDB native-engine behavior for supported algorithms |
| Standalone `CREATE INDEX` / `DROP INDEX` | ⚪&nbsp;Planned | Route through MariaDB DDL and native engine index updates |
| `CREATE TABLE ... LIKE` | ⚪&nbsp;Planned | Preserve MariaDB table definition behavior |
| `CREATE TABLE ... SELECT` | ⚪&nbsp;Planned | Preserve MariaDB statement semantics over MyLite tables |
| Schemas/databases | 🟡&nbsp;Partial | Controlled `CREATE DATABASE` and qualified table access are covered inside `datadir/`; full schema lifecycle remains planned |
| Views, triggers, and routines | ⚪&nbsp;Planned | Persist through MariaDB native metadata inside the MyLite directory where supported |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded directory ownership replaces server account management |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior |

## Rows, Indexes, And Constraints

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Fixed and variable row fields | ⚪&nbsp;Planned | Native engine storage for MariaDB row values |
| NULL columns | ⚪&nbsp;Planned | Preserve MariaDB NULL storage and comparison behavior through native engines |
| BLOB/TEXT values | ⚪&nbsp;Planned | Native engine BLOB/TEXT storage |
| Primary and secondary indexes | ⚪&nbsp;Planned | Native engine indexes |
| Unique indexes | ⚪&nbsp;Planned | MariaDB duplicate-key behavior, including nullable-key rules |
| Autoincrement | ⚪&nbsp;Planned | Native engine table-local state compatible with MariaDB expectations |
| CHECK constraints | ⚪&nbsp;Planned | Use MariaDB expression evaluation and persist metadata through native metadata paths |
| Foreign keys | ⚪&nbsp;Planned | InnoDB-compatible semantics where practical; reject unsupported cases explicitly |
| Generated columns | ⚪&nbsp;Planned | Preserve MariaDB virtual/stored generated-column behavior through storage support |
| FULLTEXT and SPATIAL indexes | ⚪&nbsp;Planned | Support only where the selected native engine and embedded profile support them |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | ⚪&nbsp;Planned | Native engine atomicity within the MyLite database directory |
| Rollback | ⚪&nbsp;Planned | Native engine rollback behavior for failed statements and transactions |
| Savepoints | ⚪&nbsp;Planned | Match MariaDB transaction behavior for supported MyLite tables |
| Crash recovery | ⚪&nbsp;Planned | Recover native engine files and logs inside the MyLite database directory |
| Multiple readers | ⚪&nbsp;Planned | Safe readers over stable committed state |
| Concurrent writers | ⚪&nbsp;Planned | Preserve the selected native engine's write-concurrency behavior |
| Cross-process unsafe writers | ➖&nbsp;Out&nbsp;of&nbsp;scope | Reject or block unsafe opens until locking and recovery prove safety |
