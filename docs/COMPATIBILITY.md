# Compatibility

MyLite compatibility is tracked by surface area, not by broad claims. MariaDB
11.8 is the primary behavior authority; MySQL behavior is additional evidence
for drop-in application expectations.

## Status Key

- ✅&nbsp;Covered: implemented and covered by committed tests.
- 🟡&nbsp;Partial: implemented with documented limits and committed tests.
- ⚪&nbsp;Planned: target behavior for an upcoming slice.
- ➖&nbsp;Out&nbsp;of&nbsp;scope: deliberately omitted from the embedded
  single-file product.

## Baseline

| Area | Target |
| --- | --- |
| MariaDB base | MariaDB 11.8 LTS, initial import ref `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7` |
| Runtime shape | Embedded in-process library, no daemon required for core use |
| Durable storage | One primary `.mylite` file, plus documented MyLite-owned recovery, lock, shared-memory, and temporary companions |
| Primary API | `libmylite` file-owned C API |
| MariaDB C API | Optional adapter, not the primary lifetime model |

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database file | 🟡&nbsp;Partial | Implemented for local files with a temporary MariaDB runtime directory that is removed on final close |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds |
| Prepared statements | ⚪&nbsp;Planned | Reusable statements with 1-based parameter binding |
| Binary-safe values | ⚪&nbsp;Planned | Explicit BLOB/TEXT byte counts; no NUL-terminated-value assumptions |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text |
| Warnings | ⚪&nbsp;Planned | MariaDB-compatible warning counts and structured warning access |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct execution exposes affected rows for non-result statements and the last insert id |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| MyLite engine registration | 🟡&nbsp;Partial | Opt-in static handler builds expose `MYLITE` through `SHOW ENGINES`; table storage remains unsupported |
| No explicit engine | ⚪&nbsp;Planned | Create a MyLite table |
| `ENGINE=MYLITE` | ⚪&nbsp;Planned | Create a MyLite table |
| `ENGINE=InnoDB` | ⚪&nbsp;Planned | Route application DDL to MyLite storage where semantics are supported |
| `ENGINE=MyISAM` | ⚪&nbsp;Planned | Route legacy application DDL to MyLite storage where semantics are supported |
| `ENGINE=Aria` | ⚪&nbsp;Planned | Route application DDL to MyLite storage where semantics are supported; never create Aria durable sidecars |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Default embedded profile does not load storage-engine plugins |

## File Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database file | 🟡&nbsp;Partial | Open/create writes and validates a versioned `.mylite` header plus an empty catalog root; durable table metadata is not stored there yet |
| Persistent `.frm` files | ➖&nbsp;Out&nbsp;of&nbsp;scope | Store table definitions in the MyLite catalog |
| Persistent InnoDB sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.ibd`, redo, undo, or independent tablespace files |
| Persistent MyISAM sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.MYD` or `.MYI` durable table files |
| Persistent Aria sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.MAI`, `.MAD`, `aria_log.*`, or Aria control state as application storage |
| MyLite-owned companions | 🟡&nbsp;Partial | Bootstrap uses a MyLite-owned temporary MariaDB runtime directory and removes it on final close |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | ⚪&nbsp;Planned | MariaDB DDL semantics without durable metadata sidecars |
| `ALTER TABLE` | ⚪&nbsp;Planned | Copy/rebuild path first; in-place algorithms later when storage supports them |
| Standalone `CREATE INDEX` / `DROP INDEX` | ⚪&nbsp;Planned | Route through MariaDB DDL and MyLite catalog/index updates |
| `CREATE TABLE ... LIKE` | ⚪&nbsp;Planned | Preserve MariaDB table definition behavior |
| `CREATE TABLE ... SELECT` | ⚪&nbsp;Planned | Preserve MariaDB statement semantics over MyLite tables |
| Schemas/databases | ⚪&nbsp;Planned | Catalog namespaces, not datadir directories |
| Views, triggers, and routines | ⚪&nbsp;Planned | Catalog-backed persistent objects after table DDL is stable |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded file ownership replaces server account management |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior |

## Rows, Indexes, And Constraints

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Fixed and variable row fields | ⚪&nbsp;Planned | Store MariaDB record-compatible row values |
| NULL columns | ⚪&nbsp;Planned | Preserve MariaDB NULL storage and comparison behavior |
| BLOB/TEXT values | ⚪&nbsp;Planned | Binary-safe overflow storage |
| Primary and secondary indexes | ⚪&nbsp;Planned | Ordered access paths over MyLite pages |
| Unique indexes | ⚪&nbsp;Planned | MariaDB duplicate-key behavior, including nullable-key rules |
| Autoincrement | ⚪&nbsp;Planned | Durable table-local state compatible with MariaDB expectations |
| CHECK constraints | ⚪&nbsp;Planned | Use MariaDB expression evaluation and persist metadata in the catalog |
| Foreign keys | ⚪&nbsp;Planned | InnoDB-compatible semantics where practical; reject unsupported cases explicitly |
| Generated columns | ⚪&nbsp;Planned | Preserve MariaDB virtual/stored generated-column behavior through storage support |
| FULLTEXT and SPATIAL indexes | ⚪&nbsp;Planned | Support only after physical or well-defined metadata-backed access paths are designed |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | ⚪&nbsp;Planned | No torn catalog or row/index publication |
| Rollback | ⚪&nbsp;Planned | Restore rows, indexes, constraints, and catalog state for failed transactions |
| Savepoints | ⚪&nbsp;Planned | Match MariaDB transaction behavior for supported MyLite tables |
| Crash recovery | ⚪&nbsp;Planned | Recover primary file and any MyLite-owned journal/WAL companions |
| Multiple readers | ⚪&nbsp;Planned | Safe readers over stable committed state |
| Concurrent writers | ⚪&nbsp;Planned | Preserve the full write-concurrency goal in storage and lock design |
| Cross-process unsafe writers | ➖&nbsp;Out&nbsp;of&nbsp;scope | Reject or block unsafe opens until locking and recovery prove safety |
