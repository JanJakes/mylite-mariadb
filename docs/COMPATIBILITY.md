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

## Validation

Compatibility evidence is grouped by the local harness documented in
[Compatibility Harness](architecture/compatibility-harness.md). The initial
groups cover public API validation, storage, crash recovery, locking, embedded
lifecycle, direct SQL, prepared statements, column metadata, large value reads,
warnings, MariaDB baseline SQL API comparison, storage-engine smoke, sidecar
gates, routed DDL/DML, initial application-schema smoke, and server-surface
policy. MariaDB MTR comparison suites and broader application-schema suites
remain planned.

## Baseline

| Area | Target |
| --- | --- |
| MariaDB base | MariaDB 11.8 LTS, initial import ref `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7` |
| Runtime shape | Embedded in-process library, no daemon required for core use |
| Durable storage | One primary `.mylite` file, plus documented MyLite-owned recovery journal, lock, shared-memory, and temporary companions |
| Primary API | `libmylite` file-owned C API |
| MariaDB C API | Optional adapter, not the primary lifetime model |

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database file | 🟡&nbsp;Partial | Implemented for local files with a temporary MariaDB runtime directory that is removed on final close |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds |
| Prepared statements | 🟡&nbsp;Partial | Reusable embedded statements with 1-based scalar parameter binding, row stepping, reset/finalize ownership, MariaDB diagnostics, and representative baseline comparison coverage |
| Binary-safe values | 🟡&nbsp;Partial | Prepared statements expose explicit TEXT/BLOB byte counts, BLOB values with embedded NUL bytes, and current-row byte-range reads for large TEXT/BLOB results |
| Column metadata | 🟡&nbsp;Partial | Prepared statements expose alias, schema/table/origin names, MariaDB-native type, flags, charset, decimals, and length metadata; representative native type/name comparison is covered, while parameter metadata remains planned |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text |
| Warnings | 🟡&nbsp;Partial | Successful direct and prepared execution expose retained `SHOW WARNINGS` rows; failed direct execution, failed prepare, and failed prepared execute retain structured error rows before a result set is active; representative baseline comparison is covered, while fetch-time failure warning capture remains planned |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct and prepared execution expose affected rows for non-result statements and the last insert id |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| MyLite engine registration | 🟡&nbsp;Partial | Opt-in static handler builds expose `MYLITE` through `SHOW ENGINES`; file-backed MyLite sessions configure it as the default effective engine |
| No explicit engine | 🟡&nbsp;Partial | `CREATE TABLE` routes to MyLite, stores requested engine `DEFAULT` with effective engine `MYLITE`, supports row insert, full scans, update/delete, truncate, copy `ALTER` rebuilds, BLOB/TEXT payloads, autoincrement, and supported primary/unique/secondary indexes, and updates catalog metadata on `DROP TABLE` and simple `RENAME TABLE`; unsupported index classes remain explicit failures |
| `ENGINE=MYLITE` | 🟡&nbsp;Partial | Explicit MyLite DDL stores MariaDB table-definition metadata in the `.mylite` catalog, rediscovers it after reopen, supports row insert, full scans, update/delete, truncate, copy `ALTER` rebuilds, BLOB/TEXT payloads, autoincrement, and supported primary/unique/secondary indexes, and updates catalog metadata on `DROP TABLE` and simple `RENAME TABLE`; unsupported index classes remain explicit failures |
| `ENGINE=InnoDB` | 🟡&nbsp;Partial | DDL routes to MyLite and records requested `InnoDB`; row insert, update/delete, truncate, copy `ALTER` rebuilds, BLOB/TEXT payloads, autoincrement, supported primary/unique/secondary indexes, full scans, `DROP TABLE`, and simple `RENAME TABLE` are covered, while InnoDB transactions, foreign keys, BLOB/TEXT indexes, and tablespaces remain unsupported |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | DDL routes to MyLite and records requested `MyISAM`; row insert, update/delete, truncate, copy `ALTER` rebuilds, BLOB/TEXT payloads, full scans, supported primary/unique/secondary indexes, `DROP TABLE`, and simple `RENAME TABLE` are covered, while MyISAM data and index files are never durable application storage |
| `ENGINE=Aria` | 🟡&nbsp;Partial | DDL routes to MyLite and records requested `Aria`; row insert, update/delete, truncate, copy `ALTER` rebuilds, BLOB/TEXT payloads, full scans, supported primary/unique/secondary indexes, `DROP TABLE`, and simple `RENAME TABLE` are covered, while Aria data, index, and log files are never durable application storage |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Unsupported explicit engine requests fail before catalog publication |

## File Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database file | 🟡&nbsp;Partial | Open/create writes and validates a versioned `.mylite` header, catalog root, table-definition records, row pages, row-payload overflow pages, row-state pages, autoincrement state pages, and index-entry pages |
| Persistent `.frm` files | ➖&nbsp;Out&nbsp;of&nbsp;scope | Store table definitions in the MyLite catalog; metadata DDL smoke tests gate against durable `.frm` sidecars |
| Persistent InnoDB sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.ibd`, redo, undo, or independent tablespace files; metadata DDL smoke tests gate against known InnoDB sidecar names |
| Persistent MyISAM sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.MYD` or `.MYI` durable table files; metadata DDL smoke tests gate against those sidecars |
| Persistent Aria sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.MAI`, `.MAD`, `aria_log.*`, or Aria control state as application storage; metadata DDL smoke tests gate against those names |
| MyLite-owned companions | 🟡&nbsp;Partial | Bootstrap uses a MyLite-owned temporary MariaDB runtime directory and requires it to be empty after final close in storage-engine smoke tests |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE` metadata | 🟡&nbsp;Partial | Store MyLite table definitions for omitted/default engine, `ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` without durable MariaDB sidecars |
| `DROP TABLE` | 🟡&nbsp;Partial | Remove MyLite catalog metadata for routed base tables without durable MariaDB sidecars; orphaned definition, row, and index-entry pages are not reclaimed yet |
| `RENAME TABLE` | 🟡&nbsp;Partial | Rename MyLite catalog metadata for simple routed base tables without durable MariaDB sidecars; index rename paths remain planned |
| `UPDATE` / `DELETE` | 🟡&nbsp;Partial | Full-scan and supported keyed update/delete are covered for routed base tables, including BLOB/TEXT payload rows and close/reopen visibility; rollback, triggers, foreign keys, generated-column edge cases, and unsupported index classes remain planned |
| `TRUNCATE TABLE` | 🟡&nbsp;Partial | Logical truncate is covered for supported routed table shapes: live rows and index entries become invisible, autoincrement resets to the first generated value, catalog metadata is preserved, and close/reopen visibility is tested; SQL rollback, foreign keys, physical compaction, and transaction-aware indexes remain planned |
| `ALTER TABLE` | 🟡&nbsp;Partial | Copy rebuilds are covered for supported MyLite-routed table shapes, including column add/drop/rename, supported index additions, BLOB/TEXT payload rows, requested-engine metadata, close/reopen visibility, and rollback-journal publication; `LOCK=NONE`, in-place algorithms, unsupported index classes, SQL rollback, and foreign keys remain planned |
| Standalone `CREATE INDEX` / `DROP INDEX` | 🟡&nbsp;Partial | Route supported copy-rebuild index additions and drops through MariaDB DDL and MyLite catalog/index updates; online DDL, unsupported index classes, SQL rollback, and foreign keys remain planned |
| `CREATE TABLE ... LIKE` | ⚪&nbsp;Planned | Preserve MariaDB table definition behavior |
| `CREATE TABLE ... SELECT` | ⚪&nbsp;Planned | Preserve MariaDB statement semantics over MyLite tables |
| Schemas/databases | ⚪&nbsp;Planned | Catalog namespaces, not datadir directories |
| Views, triggers, and routines | ⚪&nbsp;Planned | Catalog-backed persistent objects after table DDL is stable |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile; event activation and event DDL are rejected by MyLite SQL policy |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded file ownership replaces server account management; account SQL is rejected by MyLite SQL policy |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior; binlog is disabled at startup and representative replication/binlog SQL is rejected |
| Dynamic plugin installation | ➖&nbsp;Out&nbsp;of&nbsp;scope | Dynamic plugin support is compiled out of the default embedded profile, core startup uses a MyLite-owned plugin directory, and representative plugin-install SQL is rejected |
| Performance schema | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server instrumentation tables are disabled or compiled out in the default embedded runtime profile |

## Rows, Indexes, And Constraints

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Keyless table scans | 🟡&nbsp;Partial | Insert, full-scan, update, and delete rows for MyLite-routed tables without declared keys, with values persisted in the primary `.mylite` file across close/reopen |
| Fixed and variable row fields | 🟡&nbsp;Partial | Store MariaDB record-compatible row images for routed rows, including nullable fixed fields, variable fields, supported keyed rows, and BLOB/TEXT payloads covered by smoke tests |
| NULL columns | 🟡&nbsp;Partial | Preserve SQL NULL values for routed row inserts, updates, deletes, and full scans before and after reopen, including BLOB/TEXT fields and nullable unique-key semantics |
| BLOB/TEXT values | 🟡&nbsp;Partial | Routed rows persist `TEXT`/`BLOB` values in primary-file row payloads, including NULL, empty, binary, large overflow values, update/delete, and supported keyed tables whose keys do not include BLOB/TEXT parts; BLOB/TEXT indexes and binary-safe public value APIs remain planned |
| Primary and secondary indexes | 🟡&nbsp;Partial | Supported primary and secondary keys use durable append-only MyLite index-entry pages plus ordered in-memory handler cursors, including standalone copy-rebuild index DDL and logical truncate filtering; B-tree navigation, BLOB/TEXT indexes, FULLTEXT, SPATIAL, generated/expression indexes, and compaction remain planned |
| Unique indexes | 🟡&nbsp;Partial | Duplicate-key checks are covered for supported unique indexes, including nullable unique-key semantics where NULL values do not conflict unless MariaDB marks NULLs equal |
| Autoincrement | 🟡&nbsp;Partial | Durable table-local state is covered for single-column autoincrement keys, including BLOB/TEXT payload rows, explicit high values, close/reopen, supported keyed update/delete, and truncate reset; compound autoincrement edge cases, `ALTER TABLE ... AUTO_INCREMENT`, and transaction-aware rollback remain planned |
| CHECK constraints | ⚪&nbsp;Planned | Use MariaDB expression evaluation and persist metadata in the catalog |
| Foreign keys | ⚪&nbsp;Planned | InnoDB-compatible semantics where practical; reject unsupported cases explicitly |
| Generated columns | ⚪&nbsp;Planned | Preserve MariaDB virtual/stored generated-column behavior through storage support |
| FULLTEXT and SPATIAL indexes | ⚪&nbsp;Planned | Support only after physical or well-defined metadata-backed access paths are designed |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | 🟡&nbsp;Partial | Rollback journal protects current catalog, row, row-state, truncate, autoincrement, and index-entry publication; SQL transaction commit remains planned |
| Rollback | ⚪&nbsp;Planned | Restore rows, indexes, constraints, and catalog state for failed transactions |
| Savepoints | ⚪&nbsp;Planned | Match MariaDB transaction behavior for supported MyLite tables |
| Crash recovery | 🟡&nbsp;Partial | Recover the primary file from `<database>.mylite-journal` for current publication paths while holding the exclusive primary-file lock; WAL/checkpoints and transaction rollback remain planned |
| Multiple readers | 🟡&nbsp;Partial | Storage read APIs acquire shared non-blocking advisory locks over stable committed state; SQL transaction lock integration and busy-timeout behavior remain planned |
| Concurrent writers | ⚪&nbsp;Planned | Preserve the full write-concurrency goal in storage and lock design |
| Cross-process unsafe writers | 🟡&nbsp;Partial | Cooperating MyLite storage operations reject conflicting cross-process opens, writes, and recovery with busy errors; non-MyLite writers remain out of scope |

## Application Schemas

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| WordPress-shaped core tables | 🟡&nbsp;Partial | Storage-engine smoke covers a reduced `wp_options`, `wp_posts`, and `wp_postmeta` subset with `ENGINE=InnoDB` routing, autoincrement ids, `longtext` payloads, secondary indexes, joins, updates, deletes, close/reopen, and sidecar gates; full WordPress installer schema, prefix indexes, collation variants, multisite, users, comments, terms, and plugin tables remain planned |
