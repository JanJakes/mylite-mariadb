# MyLite Roadmap

This roadmap orders the first engineering slices. It tracks product work at a
higher level than per-slice specs.

## Status Key

- ✅&nbsp;Done: accepted and represented in the repository.
- ⚪&nbsp;Planned: expected work, but not started.
- 🟡&nbsp;In&nbsp;progress: active implementation or research.
- ❌&nbsp;Blocked: waiting on a named prerequisite or decision.

## Current Direction

MyLite starts from MariaDB 11.8 LTS and exposes an embedded `libmylite` API over
one primary `.mylite` file. Durable application state lives in MyLite storage,
not in a MariaDB datadir or existing engine sidecars.

## Implementation Plan

| Order | Slice | Status | Purpose |
| --- | --- | --- | --- |
| 0 | Project foundation | ✅&nbsp;Done | Define the product goal, GPL baseline, architecture direction, workflow, and initial MariaDB research. |
| 1 | Import MariaDB 11.8.6 | ✅&nbsp;Done | Import `mariadb-11.8.6` mechanically and record upstream refs. |
| 2 | Minimal embedded build | ✅&nbsp;Done | Produce a reproducible embedded build and record baseline artifact size and enabled components. |
| 3 | Embedded bootstrap | ✅&nbsp;Done | Start the MariaDB-derived runtime under MyLite-owned defaults and reject daemon-only startup surfaces. |
| 4 | Public open/close API | ✅&nbsp;Done | Add `libmylite` database handles, diagnostics, open flags, close behavior, shared-runtime lifetime, and repeated embedded restart coverage, including scheduler, locale, handler, and charset restart state. |
| 5 | SQL execution API | 🟡&nbsp;In&nbsp;progress | Add direct execution, prepared statements, bindings, columns, warnings, affected rows, and insert ids. Direct execution including statement-effect coverage, prepared-statement typed values, column metadata, warning enumeration including selected failed paths, prepared diagnostics for representative routed CHECK and generated-column execution failures, current-row large-value reads, and representative MariaDB baseline comparison are implemented; an initial opt-in MTR smoke runner exists under the compatibility-harness work, while MTR-scale comparison remains. |
| 6 | Storage engine skeleton | ✅&nbsp;Done | Register a static MyLite storage engine with controlled handler smoke coverage. |
| 7 | File header and empty catalog | ✅&nbsp;Done | Create/open a valid `.mylite` file with a versioned header and empty catalog. |
| 8 | MyLite metadata DDL and discovery | 🟡&nbsp;In&nbsp;progress | Store routed metadata in the catalog and discover it without durable MariaDB sidecars. Direct and prepared schema namespaces with basic schema options, SQL-layer catalog discovery after reopen, create/discovery, `CREATE TABLE ... LIKE`, successful supported `CREATE TABLE ... SELECT` including generated-source projections and representative duplicate-mode CTAS, representative temporary LIKE/CTAS catalog isolation, shadowing, and temporary OR REPLACE behavior, representative successful `CREATE OR REPLACE TABLE ... LIKE` and CTAS replacement, representative failed OR REPLACE rollback, `DROP TABLE`, simple and indexed `RENAME TABLE`, representative table-DDL `IF EXISTS` skip semantics, representative failed table-DDL rollback for multi-table DROP/RENAME, copy `ALTER` rebuilds including catalog-only reopened CHECK drops, generated-column add/modify/drop, representative default-algorithm reopened column/index/autoincrement ALTER, and explicit online/in-place ALTER rejection, supported keyed rebuilds, standalone supported index DDL, SQL-level supported index rename, basic CHECK and generated-column metadata persistence, named table-level CHECK add/drop ALTER, representative `SHOW CREATE TABLE` round-trip export/import, explicit non-table object rejection, explicit partition DDL rejection, and explicit foreign-key DDL rejection are implemented; catalog-backed views, triggers, routines, remaining filesystem-free schema DDL, partition metadata/routing, foreign-key metadata/enforcement, broader constraint DDL, broader OR REPLACE edge cases, and broader SQL rollback remain. |
| 9 | Sidecar lifecycle gates | ✅&nbsp;Done | Detect known MariaDB durable engine sidecars around metadata DDL, close/reopen, and failed-create cleanup. |
| 10 | Engine routing policy | ✅&nbsp;Done | Record requested engine vs. effective MyLite engine and route omitted/default, `InnoDB`, `MyISAM`, and `Aria` metadata where safe. |
| 11 | Row and index storage | 🟡&nbsp;In&nbsp;progress | Table insert, CTAS row population including generated-source projections, generated targets, CHECK-constrained targets, duplicate-mode targets, replacement CTAS targets, and representative temporary OR REPLACE CTAS targets, representative temporary CTAS row visibility and shadowing, full-scan, update/delete, truncate, copy rebuild, NULL values, BLOB/TEXT overflow payloads, generated BLOB/TEXT values, autoincrement state including `ALTER TABLE ... AUTO_INCREMENT`, supported primary/unique/secondary index entries, index preservation across table rename, bounded BLOB/TEXT prefix indexes, bounded generated BLOB/TEXT prefix indexes through initial and standalone copy-rebuild DDL, standalone supported index DDL, SQL-level supported index rename, basic CHECK enforcement including failed ADD CHECK rollback over incompatible existing rows, basic generated columns with copy ALTER add/modify/drop, ordinary generated-column secondary/unique index and generated-index DDL coverage, prepared diagnostics for representative routed CHECK and generated-column execution failures, representative CHECK/generated deterministic expression matrices, representative CHECK/generated dump-style import, representative `SHOW CREATE TABLE` round-trip export/import, generated primary-key rejection, explicit compound-only autoincrement key rejection, explicit unsupported FULLTEXT/SPATIAL/long-unique index rejection, explicit partition DDL rejection, and explicit foreign-key DDL rejection are implemented; partition-aware row/index maintenance, foreign-key enforcement, compound autoincrement allocation semantics, exhaustive CHECK/generated expression coverage, broader dump/export coverage, full BLOB/TEXT index support, MySQL-style expression-index compatibility, FULLTEXT/SPATIAL access paths, compaction, and transactional index maintenance remain. |
| 12 | Copy `ALTER` rebuilds | ✅&nbsp;Done | Table-copy rebuild support works over the current row and supported index lifecycle. |
| 13 | Primary and secondary indexes | ✅&nbsp;Done | Add append-only index-entry pages, ordered handler cursors, duplicate checks, nullable unique-key semantics, and index maintenance for supported insert/update/delete paths. |
| 14 | Transactions and recovery | 🟡&nbsp;In&nbsp;progress | Rollback-journal atomic publication and recovery are implemented for current append-only storage mutations, covered failed file-backed statements restore a statement-start header/catalog checkpoint including representative failed OR REPLACE replacement and multi-table DROP/RENAME paths, successful table-DDL `IF EXISTS` skips commit mixed missing/existing DROP/RENAME statements once, initial MariaDB statement transaction hooks drive row-DML checkpoint commit/rollback, and explicit SQL transaction-control statements are rejected until full transaction support exists; broader SQL rollback, multi-statement rollback, savepoints, WAL/checkpoints, and fully transactional engine flags remain. |
| 15 | Locking and concurrency | 🟡&nbsp;In&nbsp;progress | Advisory primary-file locks reject unsafe cross-process readers, writers, and recovery races, configured busy timeouts wait for cooperating lock conflicts before returning busy, and representative SQL locking surfaces are rejected until real SQL lock semantics exist; SQL lock integration and full concurrent writers remain. |
| 16 | Compatibility harness | 🟡&nbsp;In&nbsp;progress | Group existing public API, storage, recovery, locking, embedded lifecycle, SQL API comparison, sidecar, routed SQL, transaction-control, transaction-hooks, statement-rollback, partition, foreign-key, CHECK-constraint, generated-column, unsupported-index, and server-surface tests by compatibility surface; an opt-in embedded MTR smoke runner is implemented outside the default groups; MTR-scale comparison and broader application suites remain. |
| 17 | Application schemas | 🟡&nbsp;In&nbsp;progress | Broader WordPress-shaped core-table smoke coverage is implemented for options, posts, postmeta, users, usermeta, terms, taxonomy relationships, comments, commentmeta, and links with representative `utf8mb4_unicode_ci` defaults; WordPress 6.9.4 single-site installer DDL and representative installer seed fixtures import through omitted-engine routing; a representative collation restart/index matrix covers selected utf8mb4 and latin1 collations; full WordPress runtime install, exhaustive installer defaults/roles, exhaustive collation suites, ORM suites, multisite, and plugin schemas remain. |
| 18 | Server-surface policy | 🟡&nbsp;In&nbsp;progress | Runtime defaults disable networking, grants, binlog, events, and host plugin discovery; representative server SQL rejection smoke is implemented; the default profile compiles out dynamic plugins, Performance Schema, socket auth, feedback, and thread-pool info. |
| 19 | Size profile hardening | 🟡&nbsp;In&nbsp;progress | Archive and linked-runtime size reporting is implemented, and the first server-surface trim is measured; deeper daemon-only and low-value optional component trims remain. |

## Size And Profile Direction

MyLite should be smaller than a full MariaDB server distribution, but size is a
measured engineering constraint. The default embedded profile omits runtime
surface that does not fit a local file-owned library:

- network listener and server account administration,
- replication, binlog, relay log, and Galera/wsrep,
- dynamic plugin loading and external durable storage engines,
- performance schema and server audit plugins,
- rarely used optional engines or plugins unless a slice justifies them.

The minimal embedded build establishes the first baseline. Later slices record
meaningful size changes when they add or remove runtime surface.

Historical branch-level size research is archived in
[Bundle size reduction attempts](architecture/bundle-size-research.md). Treat
it as ranked evidence for size-profile work; rerun candidates against the
current baseline before accepting them.
