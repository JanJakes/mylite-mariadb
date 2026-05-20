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
| 5 | SQL execution API | 🟡&nbsp;In&nbsp;progress | Add direct execution, prepared statements, bindings, columns, warnings, affected rows, and insert ids. Direct and prepared statement-effect coverage including representative `ON DUPLICATE KEY UPDATE` duplicate-update insert-id behavior with `LAST_INSERT_ID(id)`, routed-storage ODKU affected-row and insert-id assertions for `INSERT ... VALUES` and `INSERT ... SELECT`, prepared-statement typed values and parameter counts, column metadata, warning enumeration including selected failed paths, prepared diagnostics for representative routed CHECK, generated-column, and FK execution failures with scalar-parameter FK checks, current-row large-value reads, prepared routed-storage SELECT coverage for aggregate, primary-key, secondary-index, text, native integer, and scalar-parameter reads, explicit direct/prepared file-import and file-export rejection, and representative MariaDB baseline comparison for values, direct/prepared/typed prepared expression result sets, metadata, warnings, and statement effects are implemented; rich prepared parameter metadata is explicitly unsupported on the current MariaDB base, and the opt-in MTR smoke runner now covers scalar CAST/CONVERT, CASE-family and ANSI SQL-mode expression behavior, selected numeric, hex-hybrid literal/rounding, integer metadata/rounding, character/varchar, binary-string, binary/varbinary, national-character, interval, and type behavior, selected date, temporal-rounding, temporal-function, alternate-timezone conversion, temporal scale, microsecond parsing, and time/datetime comparison behavior, selected parser/comment, keyword, CTE-cycle, precedence, expression, type-coercion, rounding, safe-update, and comparison behavior, selected `IN` / `NOT IN` predicate behavior, selected subquery, update-ignore, `REPLACE`, `RETURNING`, temporary create-or-replace, primary-key lookup, selected DDL/name and comment metadata behavior, selected DDL constraint/index metadata and UCA CTAS behavior, selected `ORDER BY` optimizer and aggregate-ordering behavior, optimizer-cost metadata, selected EXPLAIN plan output, UNION, prepared-statement behavior, selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior, selected scalar operator behavior, selected bit/extraction/replacement/regexp scalar-function behavior, selected default-expression and weight-string behavior, selected string/format function and charset-conversion expression behavior, selected crypto/KDF, disabled DES, and JSON equality/normalization behavior, selected aggregate DISTINCT behavior, selected autoincrement ODKU and strict HEAP autoincrement behavior, selected date-format behavior, selected ASCII, legacy, UTF-32, filesystem, Latin2, UTF-8 binary/general, and UTF-8 UCA 1400 charset edge behavior, selected charset weight-string, LIKE condition-propagation, and multibyte recoding behavior, and selected charset diagnostics, collation-default behavior, UTF-32 `character_set_collations`, and column/context collation behavior under compatibility-harness work, while MTR-scale comparison remains. |
| 6 | Storage engine skeleton | ✅&nbsp;Done | Register a static MyLite storage engine with controlled handler smoke coverage. |
| 7 | File header and empty catalog | ✅&nbsp;Done | Create/open a valid `.mylite` file with a versioned header and empty catalog. |
| 8 | MyLite metadata DDL and discovery | 🟡&nbsp;In&nbsp;progress | Store routed metadata in the catalog and discover it without durable MariaDB sidecars. Direct and prepared schema namespaces with basic schema options, SQL-layer catalog discovery after reopen, directory-free file-backed initial `CREATE DATABASE` / `CREATE SCHEMA`, catalog-backed `CREATE DATABASE` duplicate, `IF NOT EXISTS`, and `OR REPLACE` paths, create/discovery, ordinary `CREATE TABLE IF NOT EXISTS`, `CREATE TABLE ... LIKE` including FK-source no-FK-copy behavior, successful supported `CREATE TABLE ... SELECT` including generated-source projections, FK-constrained targets for the current supported FK subset, and representative duplicate-mode CTAS, representative temporary LIKE/CTAS catalog isolation, shadowing, and temporary OR REPLACE behavior, representative successful plain `CREATE OR REPLACE TABLE` including generated/CHECK replacement metadata, representative successful `CREATE OR REPLACE TABLE ... LIKE` and CTAS replacement, representative failed OR REPLACE rollback including missing-source LIKE/CTAS inputs, representative FK-aware OR REPLACE parent rejection and child replacement cleanup across plain, LIKE, and CTAS forms, `DROP TABLE`, simple and indexed `RENAME TABLE`, representative table-DDL `IF EXISTS` skip semantics, representative failed table-DDL rollback for multi-table DROP/RENAME including child FK metadata, copy `ALTER` rebuilds including catalog-only reopened CHECK drops, column ALTER existence-option skips, generated-column add/modify/drop, representative primary-key ALTER add/drop/re-add, failed ADD UNIQUE rollback, failed strict conversion ALTER rollback, representative default-algorithm reopened column/index/autoincrement ALTER, and explicit online/in-place ALTER rejection, supported keyed rebuilds, supported index DDL including existence-option skips, SQL-level supported index rename including existence-option skips, supported secondary-index ignorability including `IF EXISTS` skips, representative non-CHECK primary/unique constraint DDL including unique-constraint existence-option paths, explicit key-name matrices, and composite unique constraints, basic CHECK and generated-column metadata persistence, named table-level CHECK add/drop ALTER including existence-option skips, representative `SHOW CREATE TABLE` round-trip export/import including ALTER-evolved generated/CHECK/indexed tables and FK parent/child tables, explicit non-table object rejection, explicit partition DDL and partition-management rejection, and the first public direct/prepared FK DDL publication subset for validated `RESTRICT` / `NO ACTION` constraints over explicit or MariaDB-generated supported child keys are implemented; FK metadata persists child/parent definitions through read/list/parent-list/drop/rename coverage, referenced parent unique-key rename metadata updates, handler FK metadata hooks expose FK records through MariaDB information-schema, parent-table metadata checks, `SHOW CREATE TABLE`, FK-aware copy ALTER column/supporting-key checks with handler-owned retained-key validation under MyLite's FK handlerton advertising, copy-ALTER and successful CTAS metadata preservation including generated supporting-key cleanup, failed FK-constrained CTAS target cleanup, SQL-level referenced-parent drop rejection and child-table FK cleanup, supported `DROP FOREIGN KEY` metadata removal, immediate exact-index child/parent row checks including basic and same-row self-references plus prepared FK DML diagnostics with scalar-parameter checks, ordered multi-row child/self-reference FK checks with failed-statement rollback, representative ordered self-reference update/delete checks, representative non-self parent update/delete rollback ordering, representative multi-table parent-first/child-first FK update/delete ordering, representative parent-target multi-table FK action matrices, bounded self-referencing, same-row self-referencing, non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL`, bounded `ON DELETE CASCADE`, bounded acyclic recursive `ON UPDATE CASCADE`, supported action combinations, same-row update action matrices, explicit `SET DEFAULT` FK action rejection, self-referencing truncate, and session `foreign_key_checks=0` row-DML plus parent-truncate bypass; catalog-backed views, triggers, routines, partition metadata/routing, broader exhaustive multi-table FK update/delete matrices, cyclic or full recursive FK action graphs, broader non-CHECK constraint matrices, broader OR REPLACE edge cases, and broader SQL rollback remain. |
| 9 | Sidecar lifecycle gates | ✅&nbsp;Done | Detect known MariaDB durable engine sidecars around metadata DDL, close/reopen, and failed-create cleanup. |
| 10 | Engine routing policy | ✅&nbsp;Done | Record requested engine vs. effective MyLite engine and route omitted/default, `InnoDB`, `MyISAM`, and `Aria` metadata where safe; `BLACKHOLE` routes to MyLite metadata with row-discard behavior, `MEMORY` / `HEAP` route to durable metadata with runtime-volatile row semantics, and unsupported explicit `ENGINE` table options reject before catalog publication. |
| 11 | Row and index storage | 🟡&nbsp;In&nbsp;progress | Table insert, plain replacement target row/index/autoincrement reset, CTAS row population including generated-source projections, generated targets, CHECK-constrained targets, FK-constrained targets for the current supported FK subset, duplicate-mode targets, replacement CTAS targets, and representative temporary OR REPLACE CTAS targets, representative temporary CTAS row visibility and shadowing, full-scan, update/delete, truncate, copy rebuild, NULL values, BLOB/TEXT overflow payloads, generated BLOB/TEXT values, autoincrement state including `ALTER TABLE ... AUTO_INCREMENT`, supported primary/unique/secondary index entries including primary-key drop/re-add and failed unique-add rollback, index preservation across table rename, bounded BLOB/TEXT prefix indexes, bounded generated BLOB/TEXT prefix indexes through initial and standalone copy-rebuild DDL, standalone supported index DDL, SQL-level supported index rename, supported secondary-index ignorability metadata and hint rejection, representative primary/unique constraint-backed key DDL including explicit key-name matrices and composite and nullable composite unique constraints, generated FK child supporting keys and generated FK supporting-key cleanup, basic CHECK enforcement including ALTER existence-option skips, representative grouped ODKU CHECK rollback, and failed ADD CHECK rollback over incompatible existing rows, exact-index FK child/parent row checks over retained storage metadata and supported public FK DDL including basic and same-row self-references plus prepared FK DML diagnostics with scalar-parameter checks, retained FK supporting-key validation under MyLite's FK handlerton advertising, referenced parent unique-key rename row-check preservation, ordered multi-row child/self-reference FK checks with failed-statement rollback, representative ordered self-reference update/delete checks, representative non-self parent update/delete rollback ordering, representative multi-table parent-first/child-first FK update/delete ordering, representative parent-target multi-table FK action matrices, bounded self-referencing, same-row self-referencing, non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL`, bounded `ON DELETE CASCADE`, bounded acyclic recursive `ON UPDATE CASCADE`, supported action combinations, same-row update action matrices, session `foreign_key_checks=0` row-check bypass, fixture-backed FK dump-style out-of-order data import under the same bypass, basic generated columns with copy ALTER add/modify/drop, ordinary generated-column secondary/unique index, generated-column unique constraint including composite virtual matrix coverage and failed-add rollback, generated dependent-column drop rollback, representative generated-column DML and expression-error rollback, and generated-index DDL coverage, prepared diagnostics for representative routed CHECK and generated-column execution failures, prepared routed-storage SELECT coverage for full-scan, primary-key, secondary-index, native integer, text, and scalar-parameter reads, representative CHECK/generated deterministic string, NULL-handling, conditional, temporal, and numeric expression matrices, representative CHECK/generated dump-style import, representative `SHOW CREATE TABLE` round-trip export/import including ALTER-evolved generated/CHECK/indexed tables and FK parent/child tables, generated primary-key rejection, first-key compound autoincrement table-local allocation, live index-entry grouped per-prefix autoincrement maximum lookup including stale delete/update filtering and reverse-sort definitions, representative autoincrement offset/increment behavior including multi-row post-explicit allocation, broader offset/increment pair matrix coverage including offset greater than increment, representative integer-width autoincrement overflow boundaries through signed `BIGINT`, explicit `BIGINT UNSIGNED` maximum-value handling with generated read-failed exhaustion, direct/prepared transaction, direct savepoint, failed/ignored generated insert autoincrement gap preservation including durable first-key generated reservation gaps and source-driven failed/ignored `INSERT ... SELECT` reservation growth, explicit high-value duplicate-insert non-consumption, explicit high-value UPDATE autoincrement semantics including prior-success failed-statement preservation across representative FK, duplicate-key, and CHECK failures, representative mixed-row failed-DML autoincrement matrices, and representative `ON DUPLICATE KEY UPDATE` autoincrement reservation, failed duplicate-update reservation preservation, public statement effects, explicit-update advancement for `INSERT ... VALUES` and `INSERT ... SELECT`, including unknown source-row-count reservation growth, and grouped later-in-key per-prefix allocation, `LAST_INSERT_ID(id)`, explicit high-value advancement, source-driven `INSERT ... SELECT`, prepared `VALUES` / `INSERT ... SELECT`, failed duplicate-update rollback to live prefix maxima, source-read, update-expression, generated-expression, and CHECK-constraint error rollback, grouped failed-DML mixed insert/update rollback plus UPDATE IGNORE skips to live prefix maxima, close/reopen persistence, runtime-volatile MEMORY/HEAP autoincrement overflow rejection, explicit unsupported FULLTEXT/SPATIAL/vector/long-unique index rejection, explicit partition DDL and partition-management rejection, unsupported FK-shape and `SET DEFAULT` FK action rejection, lazy index cursor row materialization, direct row-id materialization without per-row full row-state map rebuilds, key-filtered exact/prefix cursors, storage-level exact-entry and exact-entryset lookup, single-pass durable exact-index scans, catalog-backed index root metadata, contiguous index leaf runs with page-range lookup and append-tail overlay for exact byte-key lookup, opportunistic SQL leaf-root publication for explicit fixed-width `CREATE INDEX` and `ALTER TABLE ... ADD KEY` copy rebuilds when catalog headroom allows, and a local direct/prepared performance baseline harness for routed storage are implemented; partition-aware row/index maintenance, broader exhaustive multi-table FK update/delete matrices, cyclic or full recursive FK action graphs, storage-level B-tree navigation, multi-page catalog storage, broader grouped `ON DUPLICATE KEY UPDATE` trigger, view, and exhaustive expression-error matrices, broader failed-DML autoincrement matrices, exhaustive autoincrement offset/increment integer-width matrices, exhaustive CHECK/generated expression coverage, full dump/export compatibility, full BLOB/TEXT index support, MySQL-style expression-index compatibility, FULLTEXT/SPATIAL/vector access paths, compaction, transactional index maintenance, and benchmark thresholds remain. |
| 12 | Copy `ALTER` rebuilds | ✅&nbsp;Done | Table-copy rebuild support works over the current row and supported index lifecycle. |
| 13 | Primary and secondary indexes | ✅&nbsp;Done | Add append-only index-entry pages, ordered handler cursors, duplicate checks, nullable unique-key semantics, and index maintenance for supported insert/update/delete paths. |
| 14 | Transactions and recovery | 🟡&nbsp;In&nbsp;progress | Rollback-journal atomic publication and recovery with restored-tail truncation are implemented for current append-only storage mutations, covered failed file-backed statements restore a statement-start header/catalog checkpoint including direct and prepared row-DML unique-key failures, representative direct/prepared/select-source multi-row `REPLACE` failures, representative generated-column DML and expression-error rollback, failed strict conversion copy ALTER, representative failed OR REPLACE replacement including missing-source LIKE/CTAS inputs and multi-table DROP/RENAME paths including FK metadata, successful table-DDL `IF EXISTS` skips commit mixed missing/existing DROP/RENAME statements once, initial MariaDB statement transaction hooks drive autocommit row-DML checkpoint commit/rollback, transactional engine table flags align MariaDB capability checks with bounded MyLite row-DML rollback, direct/prepared `BEGIN` / `START TRANSACTION` / `COMMIT` / `ROLLBACK`, transaction restart through repeated direct or prepared `BEGIN` / `START TRANSACTION`, supported direct/prepared `START TRANSACTION READ WRITE` / `READ ONLY`, direct and prepared session `SET autocommit=0/1/DEFAULT` forms, prepared single-marker `SET autocommit=?` transitions, mixed ordinary `SET` lists, and duplicate supported assignment lists applied in order with the final value as session state, direct/prepared `SET TRANSACTION` `READ WRITE` / `READ ONLY` and `ISOLATION LEVEL` forms, direct/prepared transaction read-only and isolation variable assignments including prepared single-marker values and duplicate supported assignment lists where the final value wins, direct/prepared session `SET completion_type=NO_CHAIN/0/DEFAULT/CHAIN/1` including prepared single-marker values and duplicate supported assignment lists where the final value wins, direct/prepared `COMMIT` / `ROLLBACK` `AND CHAIN`, `AND NO CHAIN`, and `NO RELEASE` modifiers, direct plus prepared savepoint rollback/release with case-insensitive simple unquoted, backtick-quoted, and ANSI_QUOTES double-quoted names over durable and MEMORY/HEAP volatile rows, and handler-level native MariaDB savepoint hooks for raw embedded routed durable row-DML plus representative MEMORY volatile rows, read-only transaction rejection for direct and prepared durable MyLite storage writes, direct/prepared read-only transaction row DML against tracked temporary tables, and direct/prepared explicit temporary table create/drop inside active transactions support row-DML transactions through nested MyLite checkpoints and volatile temporary storage; active row-DML transaction crash recovery through transaction journals, same-process two-handle transaction-owner read snapshots, cross-process transaction-journal read snapshots, and generated autoincrement gap preservation after direct/prepared transaction rollback, direct savepoint rollback, MEMORY/HEAP volatile statement, transaction, and savepoint rollback, failed/ignored generated inserts including durable first-key generated reservation gaps and source-driven failed/ignored `INSERT ... SELECT` reservation growth, failed `INSERT ... VALUES` and `INSERT ... SELECT` ODKU generated reservation preservation, explicit high-value duplicate-insert non-consumption, mixed generated `INSERT IGNORE` reserved tail gaps, FK-protected multi-row update non-consumption, successful explicit high-value insert/update rollback preservation, prior-success failed-update explicit autoincrement preservation across representative FK, duplicate-key, and CHECK failures, successful explicit high-value `INSERT ... VALUES` and `INSERT ... SELECT` `ON DUPLICATE KEY UPDATE` rollback preservation, and explicit rejection coverage for global autocommit, global transaction-variable, global `completion_type`, direct marker, expression-valued, global/expression parameterized transaction-control `SET`, bound `DEFAULT` / `RELEASE`, duplicate `SET TRANSACTION` characteristic forms, consistent snapshots, release completion, release completion-type defaults, XA, and durable direct or prepared DDL inside active transactions are covered; broader SQL rollback and broader failed-DML autoincrement matrices, WAL/checkpoints, and full storage isolation remain. |
| 15 | Locking and concurrency | 🟡&nbsp;In&nbsp;progress | Advisory primary-file locks reject unsafe cross-process readers, writers, and recovery races, configured busy timeouts wait for cooperating lock conflicts before returning busy, and representative SQL locking surfaces are rejected until real SQL lock semantics exist; SQL lock integration and full concurrent writers remain. |
| 16 | Compatibility harness | 🟡&nbsp;In&nbsp;progress | Group existing public API, storage, recovery, locking, embedded lifecycle, SQL API comparison including direct/prepared/typed prepared expression result sets, sidecar, routed SQL, transaction-control, transaction-hooks, statement-rollback, partition, foreign-key, foreign-key handler metadata, foreign-key create-info metadata, foreign-key DDL publication, CHECK-constraint, generated-column, unsupported-index, and server-surface including binlog/replication, replication/binlog filter assignments, binlog/replication system-variable assignments and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL file-I/O, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, view runtime, stored-program runtime, trigger runtime, dynamic UDF runtime, and non-table object tests by compatibility surface; an opt-in embedded MTR smoke runner with pass-gated run/probe workflows, probe summaries, reject cleanup, and timeout control, batched support-target build preparation, and suite-batched strict runs covers the trimmed bootstrap schema plus scalar CAST/CONVERT, CASE-family and ANSI SQL-mode expression behavior, selected numeric, hex-hybrid literal/rounding, integer metadata/rounding, character/varchar, binary-string, binary/varbinary, national-character, interval, and type behavior, selected date, temporal-rounding, temporal-function, alternate-timezone conversion, temporal scale, microsecond parsing, and time/datetime comparison behavior, selected parser/comment, keyword, CTE-cycle, precedence, expression, type-coercion, rounding, safe-update, and comparison behavior, selected `IN` / `NOT IN` predicate behavior, selected subquery, update-ignore, `REPLACE`, `RETURNING`, temporary create-or-replace, primary-key lookup, selected DDL/name and comment metadata behavior, selected DDL constraint/index metadata and UCA CTAS behavior, selected `ORDER BY` optimizer and aggregate-ordering behavior, optimizer-cost metadata, selected EXPLAIN plan output, UNION, prepared-statement behavior, selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior, selected scalar operator behavior, selected bit/extraction/replacement/regexp scalar-function behavior, selected default-expression and weight-string behavior, selected string/format function and charset-conversion expression behavior, selected crypto/KDF, disabled DES, and JSON equality/normalization behavior, selected aggregate DISTINCT behavior, selected autoincrement ODKU and strict HEAP autoincrement behavior, selected date-format behavior, selected ASCII, legacy, UTF-32, filesystem, Latin2, UTF-8 binary/general, and UTF-8 UCA 1400 charset edge behavior, selected charset weight-string, LIKE condition-propagation, and multibyte recoding behavior, and selected charset diagnostics, collation-default behavior, UTF-32 `character_set_collations`, and column/context collation behavior outside the default groups; MTR-scale comparison and broader application suites remain. |
| 17 | Application schemas | 🟡&nbsp;In&nbsp;progress | Broader WordPress-shaped core-table smoke coverage is implemented for options, posts, postmeta, users, usermeta, terms, taxonomy relationships, comments, commentmeta, and links with representative `utf8mb4_unicode_ci` defaults; WordPress 6.9.4 single-site installer DDL and installer seed fixtures import through omitted-engine routing, including the full deterministic single-site `populate_options()` option-name inventory, selected default values and autoload flags, and the full default role payload; WordPress 6.9.4 multisite global and blog-id-2 schema plus representative network seed fixtures import through omitted-engine routing with indexed network and per-blog rows; BuddyPress 14.4.0 full-component plugin schema and representative rows import through omitted-engine routing with indexed activity, notification, friends, groups, messaging, xProfile, blog-tracking, invitation, and opt-out rows; Laravel v13.6.0 default ORM schema and representative rows import through omitted-engine routing with indexed user, session, cache, job, batch, and failed-job rows; Django 6.0.5 default auth, admin, contenttypes, sessions, and migration-recorder schema plus representative rows import through omitted-engine routing with indexed migration, permission, user, session, and admin-log rows; Rails v8.1.3 Active Record metadata, Active Storage, and Action Text schema plus representative rows import through omitted-engine routing with indexed migration metadata, blob, attachment, variant, and rich-text rows; a representative collation restart/index matrix covers selected utf8mb4, utf8mb3, latin1, latin2, and cp1250 collations including MariaDB 11.8 UCA 1400 defaults; full WordPress runtime install, dynamic PHP/theme/localization installer output, exhaustive collation suites, broader ORM suites and migration runners, Laravel/Django/Rails runtime integration, full multisite runtime, additional per-blog suites, BuddyPress runtime activation, WordPress `dbDelta()` plugin execution, and additional plugin schemas remain. |
| 18 | Server-surface policy | 🟡&nbsp;In&nbsp;progress | Runtime defaults disable networking, grants, binlog, events, and host plugin discovery; representative server SQL plus replication/binlog filter assignment, binlog/replication system-variable assignment and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL host-file import/export, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection including known external no-equals engine names, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, dynamic UDF DDL, and non-table object rejection smoke is implemented; the default profile compiles out dynamic plugin loading, Performance Schema, socket auth, feedback, thread-pool info, userstat, host-file SQL I/O, unsupported server utility functions, the Oracle SQL mode parser, XML SQL functions, GIS SQL functions, vector SQL and MHNSW runtime, SFORMAT, JSON schema validation, JSON table-function execution, dynamic-column packed BLOB runtime, and the full SQL HANDLER, SQL sequence runtime, virtual SEQUENCE storage engine, SQL HELP, PROCEDURE ANALYSE, generic SELECT procedure, view runtime and sidecar metadata, stored-program runtime, event parse-data validation, trigger runtime and sidecar metadata, dynamic UDF runtime, binlog transaction/event-root, log-event server, replication GTID-state runtime, replication filter runtime, binlog/replication system-variable registration for disabled topology features, MyISAM maintenance, native CSV, InnoDB, MyISAM, MRG_MyISAM, and partition engine registration, native-MyISAM-only system variables, foreign-server metadata, external backup SQL, query cache runtime, zlib-backed compression, statement profiling class bodies, optimizer trace runtime, static SHOW information producers, status metadata producers, process-list metadata producers, and routine metadata producers. |
| 19 | Size profile hardening | 🟡&nbsp;In&nbsp;progress | Archive and linked-runtime size reporting is implemented, server-surface trims are measured, and the embedded profile now omits unsupported `LOAD DATA` / `LOAD XML` execution, SQL host-file I/O bodies, low-value server utility SQL functions, the Oracle SQL mode parser, XML SQL functions, GIS SQL functions, vector SQL and MHNSW runtime, SFORMAT, JSON schema validation, JSON table-function execution, dynamic-column packed BLOB runtime, dynamic plugin loading, and the full SQL HANDLER, SQL sequence runtime, virtual SEQUENCE storage engine, SQL HELP, PROCEDURE ANALYSE, generic SELECT procedure, view runtime and sidecar metadata, stored-program runtime and sidecar metadata, event parse-data validation, trigger runtime and sidecar metadata, dynamic UDF runtime, binlog transaction/event-root core, log-event server runtime, replication GTID-state runtime, replication filter runtime, binlog/replication system-variable registration for disabled topology features, MyISAM maintenance, native CSV, InnoDB, MyISAM, MRG_MyISAM, and partition engine registration, native-MyISAM-only system variables, user-statistics plugin, foreign-server metadata, external backup SQL runtime, query cache runtime, zlib-backed compression, statement profiling class bodies, optimizer trace runtime, static SHOW information producers, status metadata producers, process-list metadata producers, and routine metadata producers, including `gtid_index.cc`, `log_event.cc`, `log_event_server.cc`, `rpl_gtid.cc`, `rpl_filter.cc`, `rpl_injector.cc`, `rpl_record.cc`, `mi_check.c`, `mi_keycache.c`, `mi_preload.c`, `ha_innodb.cc`, `handler0alter.cc`, `srv0start.cc`, `trx0trx.cc`, `row0mysql.cc`, `dict0dict.cc`, `buf0buf.cc`, `fil0fil.cc`, `fsp0fsp.cc`, `log0log.cc`, `ha_myisam.cc`, `mi_open.c`, `ft_myisam.c`, `ha_myisammrg.cc`, `myrg_open.c`, `ha_tina.cc`, `transparent_file.cc`, `ha_partition.cc`, `item_vectorfunc.cc`, `vector_mhnsw.cc`, `json_schema.cc`, `json_table.cc`, `ma_dyncol.c`, `sql_handler.cc`, `sql_sequence.cc`, `ha_sequence.cc`, `sequence.cc`, `sql_view.cc`, `event_parse_data.cc`, `sql_trigger.cc`, `userstat.cc`, `sql_servers.cc`, `backup.cc`, `sql_cache.cc`, `emb_qcache.cc`, `opt_trace.cc`, `authors.h`, and `contributors.h` in the disabled profile, with retained `sql_embedded` C++ sources compiled without exceptions, linked first-party embedded smoke binaries no longer linking libz, and generated embedded configs clearing the host dynamic-loader probes; deeper daemon-only and low-value optional component trims remain. |

Recent row and index storage performance slices include lazy handler row
materialization from index cursors, deferred durable row-page validation for
index entryset scans, direct row-id materialization without full row-state map
rebuilds per selected row, bound searches over sorted in-memory cursors, and
pre-materialization `index_next_same()` key checks. Exact-key and prefix reads
now build filtered cursors, with a guarded first-match fast path for
non-nullable full-key unique integer lookups plus storage-level exact-entryset
lookup for matching non-unique integer reads. Durable exact lookups now scan the
published append-only page history once and prune candidates as later row-state
pages hide source row ids. These reduce current scan-based point lookup overhead
without changing the planned B-tree navigation work. Catalog-backed index-root
metadata is now available as the publication point for future navigable index
pages, and contiguous leaf runs now serve exact byte-key base snapshots with
page-range lookup and append-tail visibility overlay. SQL copy-rebuild DDL now
opportunistically
publishes those roots for explicit fixed-width `CREATE INDEX` and
`ALTER TABLE ... ADD KEY` paths when catalog headroom allows. Active checkpoint
write amortization now reuses one statement recovery journal, defers header
publication to checkpoint boundaries, and caches guarded exact duplicate-key
probes on the outer active checkpoint across nested libmylite statement
checkpoints. This materially improves explicit fixed-width index publication
and routed indexed insert throughput. Active checkpoint and snapshot header
reads now reuse the decoded in-memory header instead of re-encoding and
re-checksumming page `0`; nested write checkpoints now clone the parent
header/catalog snapshot instead of revalidating the same active pages for every
prepared row-DML savepoint. Active statements now reuse validated catalog root
pages until catalog writes or catalog-generation header changes invalidate the
statement chain. Active row mutation publication now updates decoded checkpoint
headers directly instead of encode/decode round-tripping page `0`.
Durable handler index cursor builds now keep one scoped read session open
across exact-index lookup and row materialization, reusing the validated
primary-file header and catalog root for point lookups and exact-index cursor
builds. Repeated read statements over unchanged durable header bytes and the
same file identity also reuse a thread-local decoded checkpoint snapshot after
raw header-page comparison, avoiding repeated header/catalog checksum
validation and redundant catalog-page reads on hot point-select loops.
Normal read statements now reuse a thread-local unlocked read file handle after
device/inode validation, reducing repeated `fopen()` overhead without holding
shared locks between cursor builds. That read handle now stores its device and
inode when cached, avoiding repeated `fstat()` during reuse, and read startup
reuses deterministic journal path strings while still checking both journal
paths each time. Fixed-size random page reads and writes now use
offset-addressed `pread()` / `pwrite()` calls, avoiding per-page stdio seek and
refill overhead while leaving sequential journal writes on the stream path.
MariaDB table-discovery callbacks now use the same scoped read sessions for
catalog table-definition, table-list, and existence reads, reducing repeated
prepared statement table-open validation before cursor execution.
File-backed `libmylite` sessions now default `use_stat_tables=NEVER`, avoiding
repeated MariaDB persistent `mysql.*_stats` table discovery for ordinary query
planning until MyLite has file-owned statistics.
Direct and prepared SQL entry points now summarize token-wide unsupported
surface checks through one shared dispatch helper while preserving the existing
command-specific diagnostic priority, reducing repeated MyLite policy preflight
work before MariaDB execution.
Fixed-record durable exact unique-key cursor construction now resolves the
index key and materializes the row payload through one storage operation,
avoiding a second open/header/catalog pass for primary-key point reads. Those
single-entry exact unique cursors now also keep small key, entry, and row-offset
metadata inline in the handler, avoiding per-lookup heap allocation on the
primary-key point-read path.
Fixed-width prepared result statements now reuse their result bindings across
reset/re-execute loops and avoid freeing already-drained results a second time,
leaving parameter binding/reset semantics as the next prepared-path bottleneck.
Prepared parameter bindings now follow SQLite-style reset semantics: reset keeps
bindings for reuse, clear-bindings releases them explicitly, and repeated
same-type scalar binds avoid redundant MariaDB `mysql_stmt_bind_param()` calls.
Durable exact-index cache reads now bulk-grow matching entrysets in one pass,
removing per-match array reallocations from many-match secondary cursors that do
not use published leaf roots. Exact-index caches now add transient hash buckets
for repeated exact probes, avoiding full cache scans while preserving result
order.
Handler index cursors now materialize
storage-filtered row ids without repeating per-row row-state visibility scans,
improving exact secondary reads that return many rows. Durable handler row
statistics now use stat-free nonzero estimates for optimizer planning instead
of exact table row scans or primary-file filesystem stats; SQL `COUNT(*)` and
storage row-count APIs remain exact. A small transient durable exact-index read
cache now amortizes
repeated primary-key and secondary exact lookups for append-only indexes without
published roots while preserving active-checkpoint visibility rules. Active
checkpoint exact-index caches are now maintained across update/delete
mutations, can seed from matching durable exact-index caches, and publish
maintained complete cache views on top-level commit; update/delete validation
uses direct row-id visibility checks instead of rebuilding full row-state maps.
Active exact-index caches now keep lookup buckets valid across row replacement
and delete maintenance, tombstoning removed entries and compacting only after
dead entries outnumber live entries. Those caches now keep a separate
transient row-id bucket index for source-row invalidation, avoiding a full
exact-index cache scan per maintained replacement/delete.
Transient row-state maps now hash hidden source row ids, removing the
per-candidate linear visibility lookup that dominated full scans after many
updates, and full rowset reads now collect live row ids in one file pass before
materializing only surviving row pages. SQLite-like
row-write and point-read performance still requires the planned navigable index
and pager work. Durable table-local row, payload, exact-index, and published
leaf-page caches now retarget across unrelated table row mutations, so one
table's insert/update/delete does not strand another table's hot durable cache
solely because the file page count advanced. Durable handler
index cursors also materialize their current row payloads in one ordered batch,
removing repeated per-row file open/header/catalog overhead from secondary
cursor reads while maintained navigable indexes are still pending. Active
checkpoints now cache row ids proven live by the active view, plus a stronger
payload-validated row-id set, so handler-driven update/delete validation avoids
old row-page rereads when the row payload was already validated and avoids
rescanning later row-state pages for visibility-only index proofs. Active
checkpoints also cache indexed row payloads after materialization and replace
those cached payloads after successful updates, reducing repeated row-page
reads in transaction-local update loops. Active row-payload bucket maintenance
now replaces row ids and swap-removes deleted entries without a full bucket
rebuild per cached update/delete. Non-active durable indexed-row reads
now cache row payloads by file header fingerprint,
reducing repeated secondary cursor row-page checksums. Non-active durable
full-row and count reads now cache compacted live row-id lists by durable
header fingerprint, avoiding repeated append-history scans for unchanged
checkpoints after an initial count or scan. Active transactions can now maintain
those seeded live row-id lists through insert/update/delete mutations and
publish the maintained list on commit, avoiding the first post-commit live-row
scan when the pre-transaction checkpoint was already cached. The
common inline update path now writes replacement row, row-state, and
replacement index-entry pages as one contiguous append run, reducing per-update
write syscall overhead without changing the durable page format. Active
checkpoints now buffer bounded contiguous append runs across nested statement
commits with a 16 MiB transient flush window and flush them before top-level
header publication, reducing large transaction update syscall overhead while
preserving savepoint rollback by flushing retained prefixes before truncation.
Durable updates now omit unchanged replacement index-entry pages and let
exact/live index overlays inherit unchanged keys through row-state replacement
ids, reducing write volume for any update where one or more indexed key images
stay stable. The handler also reuses that changed-key vector to skip
duplicate-key probes for unchanged unique keys on updates, avoiding repeated
exact-index checks for stable primary keys while preserving checks for inserts
and changed unique keys.
Repeated active updates of inline replacement rows now reuse the same row id and
rewrite buffered unpublished row and changed index-entry pages while those
pages are still resident in the active append buffer, including replacement
rows created by earlier nested statements in the same outer checkpoint.
Per-statement buffered-page preimages preserve savepoint rollback, and buffered
rewrite validation skips redundant full-page checksum scans for unpublished
in-memory row and index pages. Row-state pages keep a first-use checksum guard
and then use a hash-backed validated-row cache for later rewrites. Validated
row and changed index-entry pages are now encoded directly in the active append
buffer, removing the temporary read/write page copies from repeated buffered
update rewrites. Those in-buffer rewrites refresh only the row payload or index
key bytes plus any shrunken stale tail instead of rebuilding the whole page
image before recomputing the page checksum. Buffered row and index-entry undo
preimages now copy only the meaningful checksummed prefix and restore an
implicit zero tail, reducing repeated nested-statement rollback bookkeeping.
Those repeated buffered rewrites now also leave row and index-entry checksums
dirty until a generic read or buffer flush needs a valid checksum, avoiding
per-update checksum recomputation while pages remain unpublished.
Already-flushed replacement runs keep the append-only path.
Capacity failures from physical
primary-file writes, sequential journal writes, flushes, syncs, and truncation
now surface as storage-full errors instead of crashed-table I/O errors. Fresh
page encoders and full-page validators now compute the same FNV checksum with
checksum-free byte spans around the stored checksum slot, and fresh encoders
still skip known-zero tails, reducing append-page and validation CPU cost
without changing stored checksum values. The
internal rowset builder avoids per-row metadata reallocations during known
indexed-row batches, with a small row-id index keeping payload cache hits from
becoming another per-row scan. Row-id batch materialization now reuses the same
durable row-payload cache for all selected rows, avoiding repeated
filename/header/table cache discovery while the cache set generation remains
stable and preserving the same durable-view guards. BLOB/TEXT index cursors
stay on the lazy per-row payload path until batch materialization owns BLOB
payload lifetimes explicitly. Published index leaf pages
are also cached by durable file header fingerprint so repeated exact leaf-root
lookups avoid repeating leaf page reads, checksums, and decode work.
Indexed row materialization now resolves active and durable row-payload cache
availability once per batch and reuses durable cache pointers while the cache
generation remains stable, reducing repeated cache-control work for many-row
secondary exact reads.
Handler instances now cache proven child and parent foreign-key metadata
absence for opened tables, removing repeated no-op FK catalog scans from
ordinary non-FK row-DML paths, with successful local table DDL invalidating
already-open handler caches through a process-wide FK metadata epoch. Published
leaf-root exact reads now bulk-grow matching entrysets per leaf page, removing
per-row-id array reallocations from many-match secondary lookups.

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
