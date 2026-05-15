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
lifecycle, direct SQL including statement effects, prepared statements, column
metadata, large value reads, warnings, MariaDB baseline SQL API comparison,
storage-engine smoke, sidecar gates, routed DDL/DML including schema namespaces,
`CREATE TABLE ... LIKE`, `CREATE TABLE ... SELECT`, representative OR REPLACE
replacement, standalone index DDL,
index rename DDL, transaction-control policy, foreign-key DDL rejection, CHECK
constraint enforcement, generated column coverage, unsupported index-class
rejection including long-unique hash keys, partition DDL rejection, MariaDB statement transaction hook
integration, busy-timeout lock waits, SQL locking policy rejection, failed
statement rollback, initial application-schema smoke,
unsupported server/non-table-object policy, and representative
`SHOW CREATE TABLE` round-trip export/import.
MariaDB MTR comparison suites and broader application-schema suites remain
planned.

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
| Open and close a database file | 🟡&nbsp;Partial | Implemented for local files with a temporary MariaDB runtime directory that is removed on final close; repeated same-process open/close cycles and two-handle shared-runtime lifetime are covered |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks and direct statement-effect coverage in embedded builds |
| Prepared statements | 🟡&nbsp;Partial | Reusable embedded statements with 1-based scalar parameter binding, row stepping, reset/finalize ownership, MariaDB diagnostics including representative routed CHECK and generated-column execution failures, and representative baseline comparison coverage |
| Binary-safe values | 🟡&nbsp;Partial | Prepared statements expose explicit TEXT/BLOB byte counts, BLOB values with embedded NUL bytes, and current-row byte-range reads for large TEXT/BLOB results |
| Column metadata | 🟡&nbsp;Partial | Prepared statements expose alias, schema/table/origin names, MariaDB-native type, flags, charset, decimals, and length metadata; representative native type/name comparison is covered, while parameter metadata remains planned |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text |
| Warnings | 🟡&nbsp;Partial | Successful direct and prepared execution expose retained `SHOW WARNINGS` rows; failed direct execution, failed prepare, and failed prepared execute retain structured error rows before a result set is active; representative baseline comparison is covered, while fetch-time failure warning capture remains planned |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct and prepared execution expose affected rows for non-result statements and the last insert id; direct execution covers temporary-table insert/update/delete effects, and prepared execution covers parameterized insert effects |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| MyLite engine registration | 🟡&nbsp;Partial | Opt-in static handler builds expose `MYLITE` through `SHOW ENGINES`; file-backed MyLite sessions configure it as the default effective engine |
| No explicit engine | 🟡&nbsp;Partial | `CREATE TABLE` routes to MyLite, stores requested engine `DEFAULT` with effective engine `MYLITE`, supports row insert, full scans, update/delete, truncate, copy `ALTER` rebuilds, `CREATE TABLE ... LIKE`, successful supported `CREATE TABLE ... SELECT`, BLOB/TEXT payloads, autoincrement, supported primary/unique/secondary indexes, and bounded BLOB/TEXT prefix indexes, and updates catalog metadata on `DROP TABLE` and simple `RENAME TABLE`; unsupported index classes remain explicit failures |
| `ENGINE=MYLITE` | 🟡&nbsp;Partial | Explicit MyLite DDL stores MariaDB table-definition metadata in the `.mylite` catalog, rediscovers it after reopen, supports row insert, full scans, update/delete, truncate, copy `ALTER` rebuilds, `CREATE TABLE ... LIKE`, successful supported `CREATE TABLE ... SELECT`, BLOB/TEXT payloads, autoincrement, supported primary/unique/secondary indexes, and bounded BLOB/TEXT prefix indexes, and updates catalog metadata on `DROP TABLE` and simple `RENAME TABLE`; unsupported index classes remain explicit failures |
| `ENGINE=InnoDB` | 🟡&nbsp;Partial | DDL routes to MyLite and records requested `InnoDB`; row insert, update/delete, truncate, copy `ALTER` rebuilds, `CREATE TABLE ... LIKE`, successful supported `CREATE TABLE ... SELECT`, BLOB/TEXT payloads, autoincrement, supported primary/unique/secondary indexes, bounded BLOB/TEXT prefix indexes, basic CHECK constraints, basic generated columns with ordinary generated-column secondary/unique indexes, full scans, failed row-DML statement rollback through MariaDB statement hooks, `DROP TABLE`, and simple `RENAME TABLE` are covered, while InnoDB transactions, foreign-key enforcement, MySQL-style expression indexes, FULLTEXT/SPATIAL access paths, full BLOB/TEXT indexes, and tablespaces remain unsupported; generated primary keys inherit MariaDB rejection, and foreign-key DDL plus unsupported index classes including long-unique hash keys are rejected explicitly |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | DDL routes to MyLite and records requested `MyISAM`; row insert, update/delete, truncate, copy `ALTER` rebuilds, `CREATE TABLE ... LIKE`, successful supported `CREATE TABLE ... SELECT`, BLOB/TEXT payloads, full scans, supported primary/unique/secondary indexes, bounded BLOB/TEXT prefix indexes, `DROP TABLE`, and simple `RENAME TABLE` are covered, while MyISAM data and index files are never durable application storage |
| `ENGINE=Aria` | 🟡&nbsp;Partial | DDL routes to MyLite and records requested `Aria`; row insert, update/delete, truncate, copy `ALTER` rebuilds, `CREATE TABLE ... LIKE`, successful supported `CREATE TABLE ... SELECT`, BLOB/TEXT payloads, full scans, supported primary/unique/secondary indexes, bounded BLOB/TEXT prefix indexes, `DROP TABLE`, and simple `RENAME TABLE` are covered, while Aria data, index, and log files are never durable application storage |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Unsupported explicit engine requests fail before catalog publication |

## File Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database file | 🟡&nbsp;Partial | Open/create writes and validates a versioned `.mylite` header, catalog root, table-definition records, row pages, row-payload overflow pages, row-state pages, autoincrement state pages, and index-entry pages |
| Persistent `.frm` files | ➖&nbsp;Out&nbsp;of&nbsp;scope | Store table definitions in the MyLite catalog; metadata DDL smoke tests gate against durable `.frm` sidecars |
| Persistent InnoDB sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.ibd`, redo, undo, or independent tablespace files; metadata DDL smoke tests gate against known InnoDB sidecar names |
| Persistent MyISAM sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.MYD` or `.MYI` durable table files; metadata DDL smoke tests gate against those sidecars |
| Persistent Aria sidecars | ➖&nbsp;Out&nbsp;of&nbsp;scope | No `.MAI`, `.MAD`, `aria_log.*`, or Aria control state as application storage; metadata DDL smoke tests gate against those names |
| MyLite-owned companions | 🟡&nbsp;Partial | Bootstrap uses a MyLite-owned temporary MariaDB runtime directory, keeps active-runtime schema directories only for MariaDB paths that still need them, answers reopened schema discovery and representative default-algorithm copy `ALTER` paths from catalog records without rehydrating those directories, and requires the runtime directory to be empty after final close in storage-engine smoke tests |

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE` metadata | 🟡&nbsp;Partial | Store MyLite table definitions for omitted/default engine, `ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` without durable MariaDB sidecars, and cover representative catalog-backed `SHOW CREATE TABLE` export/import round trips |
| `DROP TABLE` | 🟡&nbsp;Partial | Remove MyLite catalog metadata for routed base tables without durable MariaDB sidecars; orphaned definition, row, and index-entry pages are not reclaimed yet |
| `RENAME TABLE` | 🟡&nbsp;Partial | Rename MyLite catalog metadata for simple routed base tables without durable MariaDB sidecars, preserving supported index reads and duplicate checks across close/reopen; foreign-key, trigger, partition, and broader rename semantics remain planned |
| `UPDATE` / `DELETE` | 🟡&nbsp;Partial | Full-scan and supported keyed update/delete are covered for routed base tables, including BLOB/TEXT payload rows, failed-statement rollback for covered unique-key update failures, and close/reopen visibility; multi-statement rollback, triggers, foreign keys, generated-column edge cases, and unsupported index classes remain planned |
| `TRUNCATE TABLE` | 🟡&nbsp;Partial | Logical truncate is covered for supported routed table shapes: live rows and index entries become invisible, autoincrement resets to the first generated value, catalog metadata is preserved, and close/reopen visibility is tested; SQL rollback, foreign keys, physical compaction, and transaction-aware indexes remain planned |
| `ALTER TABLE` | 🟡&nbsp;Partial | Copy rebuilds are covered for supported MyLite-routed table shapes, including column add/drop/rename, generated-column add/modify/drop, supported base and generated-column index additions, drops, and renames, BLOB/TEXT payload rows, requested-engine metadata, close/reopen visibility, catalog-only reopened CHECK drop, failed ADD CHECK rollback over incompatible existing rows, representative catalog-only reopened default-algorithm column/index/autoincrement ALTER, and rollback-journal publication; `LOCK=NONE`, in-place algorithms, unsupported index classes, broader SQL rollback, and foreign keys remain planned |
| Partitioned tables | 🟡&nbsp;Partial | `CREATE TABLE ... PARTITION BY ...` and representative partition-management `ALTER TABLE` forms are rejected before MariaDB execution until MyLite has partition metadata, partition routing, per-partition catalog lifecycle, and partition-aware row/index maintenance |
| Standalone `CREATE INDEX` / `DROP INDEX` | 🟡&nbsp;Partial | Route supported copy-rebuild index additions and drops through MariaDB DDL and MyLite catalog/index updates, including generated-column indexes and representative default-algorithm standalone index DDL after catalog-only reopen; online DDL, unsupported index classes, SQL rollback, and foreign keys remain planned |
| `ALTER TABLE ... RENAME INDEX` | 🟡&nbsp;Partial | Route supported copy-rebuild base and generated-column secondary-index renames through MariaDB ALTER and MyLite catalog/index updates, preserving forced-index reads, duplicate-key checks, and close/reopen discovery; primary-key rename, conflict matrices, online DDL, unsupported index classes, SQL rollback, and foreign keys remain planned |
| `CREATE TABLE ... LIKE` | 🟡&nbsp;Partial | Clone supported MyLite-routed table definitions without copying source rows, preserve source requested-engine metadata when no explicit engine is specified, reset target autoincrement state, cover cloned supported indexes before and after close/reopen, and cover successful representative `CREATE OR REPLACE TABLE ... LIKE`; representative temporary-table LIKE catalog isolation is covered, while unsupported source objects, foreign keys, partitions, broader temporary-table edge cases, failed replacement rollback, and SQL rollback remain planned |
| `CREATE TABLE ... SELECT` | 🟡&nbsp;Partial | Successful supported CTAS creates MyLite catalog metadata and inserts SELECT result rows through the normal handler write path, including no-engine and explicit `ENGINE=InnoDB` targets, BLOB/TEXT payloads, generated-source projections, explicit generated and CHECK-constrained target definitions, autoincrement state, supported indexes, close/reopen visibility, sidecar gates, and successful representative `CREATE OR REPLACE TABLE ... SELECT`; duplicate-key and CHECK-violation CTAS abort removes target catalog metadata, statement checkpoints now restore covered failed statement visibility, and representative temporary CTAS catalog isolation is covered, while `IGNORE` / `REPLACE`, unsupported source objects, foreign keys, partitions, broader temporary-table edge cases, failed replacement rollback, and broader SQL rollback remain planned |
| Schemas/databases | 🟡&nbsp;Partial | Direct and prepared `CREATE DATABASE` / `CREATE SCHEMA` store catalog namespace records with default character set, collation, and comment options, file-backed reopen uses SQL-layer catalog hooks so `USE`, `SHOW DATABASES`, `SHOW TABLES`, `SHOW CREATE DATABASE`, information-schema schema options, table resolution, and representative default-algorithm copy `ALTER` paths work without rehydrated runtime schema directories, and direct and prepared `DROP DATABASE` / `DROP SCHEMA` remove covered catalog metadata; broader filesystem-free schema DDL, non-table objects, and SQL rollback remain planned |
| Views, triggers, and routines | 🟡&nbsp;Partial | Catalog-backed persistent objects remain planned after table DDL is stable; current `libmylite` entry points reject representative view, trigger, routine, package, sequence, and `CALL` statements before MariaDB can publish filesystem or server-table metadata |
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
| BLOB/TEXT values | 🟡&nbsp;Partial | Routed rows persist `TEXT`/`BLOB` values in primary-file row payloads, including NULL, empty, binary, large overflow values, generated BLOB/TEXT values, update/delete, and bounded BLOB/TEXT prefix indexes; unbounded unique BLOB/TEXT long-unique hash indexes reject before catalog publication, while full BLOB/TEXT index support and broader binary-safe public value APIs remain planned |
| Primary and secondary indexes | 🟡&nbsp;Partial | Supported primary and secondary keys use durable append-only MyLite index-entry pages plus ordered in-memory handler cursors, including standalone copy-rebuild index DDL, bounded BLOB/TEXT prefix indexes, bounded generated BLOB/TEXT prefix indexes through initial and standalone copy-rebuild DDL, ordinary generated-column secondary/unique indexes, and logical truncate filtering; generated primary-key DDL inherits MariaDB rejection before catalog publication; unbounded unique BLOB/TEXT long-unique hash keys, FULLTEXT, and SPATIAL indexes reject before catalog publication; B-tree navigation, full BLOB/TEXT index support, MySQL-style expression-index compatibility, physical FULLTEXT/SPATIAL access paths, and compaction remain planned |
| Unique indexes | 🟡&nbsp;Partial | Duplicate-key checks are covered for supported unique indexes, including nullable unique-key semantics where NULL values do not conflict unless MariaDB marks NULLs equal |
| Autoincrement | 🟡&nbsp;Partial | Durable table-local state is covered for single-column autoincrement keys, including BLOB/TEXT payload rows, explicit high values, `ALTER TABLE ... AUTO_INCREMENT` before and after catalog-only reopen, preservation across copy `ALTER`, close/reopen, supported keyed update/delete, truncate reset, and explicit rejection of compound-only autoincrement key definitions before catalog publication; compound autoincrement allocation semantics and transaction-aware rollback remain planned |
| CHECK constraints | 🟡&nbsp;Partial | Basic column-level and named table-level CHECK constraints on routed tables are enforced by MariaDB before MyLite handler writes, survive close/reopen through catalog-backed table-definition metadata, honor `check_constraint_checks=OFF`, support named table-level CHECK add/drop through copy `ALTER` including reopened drops of ALTER-added checks, cover failed ADD CHECK rollback over incompatible existing rows, prepared execution diagnostics, representative deterministic expression matrices, representative dump-style import, representative `SHOW CREATE TABLE` round-trip export/import, and explicit CHECK-constrained CTAS targets, and roll back earlier rows in covered failed multi-row statements; exhaustive expression, broader dump/export, and multi-statement rollback coverage remains planned |
| Foreign keys | 🟡&nbsp;Partial | `libmylite` rejects `CREATE TABLE` and `ALTER TABLE` foreign-key DDL before MariaDB execution so routed `ENGINE=InnoDB` tables do not imply referential-integrity support; InnoDB-compatible metadata, enforcement, cascading actions, and transaction-aware checks remain planned |
| Generated columns | 🟡&nbsp;Partial | Basic virtual and stored generated columns on routed tables are handled through MariaDB generated-column evaluation, MyLite row storage, and catalog-backed table-definition metadata across insert, update, full-scan read, generated-source CTAS projection, generated target CTAS definitions, copy-rebuild add/modify/drop, representative deterministic expression matrices, representative dump-style import, representative `SHOW CREATE TABLE` round-trip export/import, and close/reopen; ordinary scalar generated-column secondary/unique indexes and bounded generated BLOB/TEXT prefix indexes are covered for initial DDL, ALTER-backed additions, standalone create/drop, rename, forced-index reads, duplicate checks, prepared execution diagnostics, update/delete maintenance, and reopen; generated primary-key create/alter DDL inherits MariaDB rejection without catalog publication, and generated unbounded BLOB/TEXT long-unique hash indexes reject before catalog publication; exhaustive expressions, broader dump/export, and rollback remain planned |
| FULLTEXT, SPATIAL, and long-unique indexes | 🟡&nbsp;Partial | Initial routed `CREATE TABLE` definitions with FULLTEXT, SPATIAL, or unbounded unique BLOB/TEXT long-unique hash indexes reject before MyLite catalog publication, and long-unique standalone/copy-rebuild additions leave existing tables unchanged; physical or well-defined metadata-backed access paths remain planned |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | 🟡&nbsp;Partial | Rollback journal protects current catalog, row, row-state, truncate, autoincrement, and index-entry publication; statement checkpoints protect covered failed file-backed SQL statements; row-DML checkpoints now commit through MariaDB statement transaction hooks; explicit SQL transaction control is rejected until full transaction support exists |
| Rollback | 🟡&nbsp;Partial | Covered failed file-backed statements restore the pre-statement MyLite header/catalog snapshot, including rows, row-state pages, index entries, autoincrement pages, and catalog records appended after the checkpoint; covered row-DML rollback is driven by MariaDB statement transaction hooks, and failed ADD CHECK copy ALTER rollback is covered for incompatible existing rows, while explicit `ROLLBACK` and rollback-to-savepoint are still rejected before MariaDB execution and full transaction/savepoint rollback remains planned |
| Savepoints | 🟡&nbsp;Partial | `SAVEPOINT` and `RELEASE SAVEPOINT` are rejected before MariaDB execution; MariaDB-compatible savepoint behavior for supported MyLite tables remains planned |
| Crash recovery | 🟡&nbsp;Partial | Recover the primary file from `<database>.mylite-journal` for current publication paths while holding the exclusive primary-file lock; WAL/checkpoints and transaction rollback remain planned |
| Multiple readers | 🟡&nbsp;Partial | Storage read APIs acquire shared advisory locks over stable committed state; configured busy timeouts wait for cooperating primary-file lock conflicts before returning busy, while SQL transaction lock integration remains planned |
| SQL locking statements | 🟡&nbsp;Partial | `LOCK TABLES`, `UNLOCK TABLES`, locking reads such as `SELECT ... FOR UPDATE` and `LOCK IN SHARE MODE`, and representative named-lock functions are rejected before MariaDB execution until table, row, named-lock, and transaction-aware lock semantics exist |
| Concurrent writers | ⚪&nbsp;Planned | Preserve the full write-concurrency goal in storage and lock design |
| Cross-process unsafe writers | 🟡&nbsp;Partial | Cooperating MyLite storage operations reject conflicting cross-process opens, writes, and recovery with busy errors after the configured timeout expires; non-MyLite writers remain out of scope |

## Application Schemas

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| WordPress-shaped core tables | 🟡&nbsp;Partial | Storage-engine smoke covers a broader `wp_options`, `wp_posts`, `wp_postmeta`, `wp_users`, `wp_usermeta`, `wp_terms`, `wp_term_taxonomy`, `wp_term_relationships`, `wp_comments`, `wp_commentmeta`, and `wp_links` subset with `ENGINE=InnoDB` routing, `utf8mb4_unicode_ci` defaults, autoincrement ids, text payloads, composite keys, secondary and prefix indexes, joins, updates, deletes, close/reopen, and sidecar gates; it also imports WordPress 6.9.4 single-site installer DDL and representative installer seed fixtures with omitted-engine routing to MyLite; full WordPress runtime install, exhaustive installer defaults/roles, exhaustive collation variants, multisite, and plugin tables remain planned |
| Representative collation index paths | 🟡&nbsp;Partial | Storage-engine smoke covers `utf8mb4_general_ci`, `utf8mb4_bin`, `utf8mb4_unicode_ci`, `utf8mb4_unicode_520_ci`, and `latin1_swedish_ci` table metadata, duplicate-key checks, indexed lookups, close/reopen, and sidecar gates; exhaustive collation semantics and locale-specific comparison matrices remain planned |
