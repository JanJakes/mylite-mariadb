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
| 5 | SQL execution API | 🟡&nbsp;In&nbsp;progress | Add direct execution, prepared statements, bindings, columns, warnings, affected rows, and insert ids. Direct and prepared statement-effect coverage including representative `ON DUPLICATE KEY UPDATE` duplicate-update insert-id behavior with `LAST_INSERT_ID(id)`, routed-storage ODKU affected-row and insert-id assertions for `INSERT ... VALUES` and `INSERT ... SELECT`, prepared-statement typed values and parameter counts, column metadata, warning enumeration including selected failed paths, prepared diagnostics for representative routed CHECK, generated-column, and FK execution failures with scalar-parameter FK checks, current-row large-value reads, prepared routed-storage SELECT coverage for aggregate, primary-key, secondary-index, text, native integer, and scalar-parameter reads, explicit direct/prepared file-import and file-export rejection, and representative MariaDB baseline comparison for values, direct/prepared/typed prepared expression result sets, metadata, warnings, and statement effects are implemented; rich prepared parameter metadata is explicitly unsupported on the current MariaDB base, and the opt-in MTR smoke runner now covers no-default-database connection state, scalar CAST/CONVERT, CASE-family and ANSI SQL-mode expression behavior, selected numeric, hex-hybrid literal/rounding, integer metadata/rounding, character/varchar, binary-string, binary/varbinary, national-character, interval, and type behavior, selected date, temporal-rounding, temporal-function, alternate-timezone conversion, system-timezone behavior, temporal literal behavior, temporal scale, high-resolution temporal function, microsecond parsing, and time/datetime comparison behavior, selected parser/comment, alias, wildcard-alias, keyword, CTE-cycle, precedence, expression, type-coercion, rounding, safe-update, negation-elimination, and comparison behavior, selected boolean aggregate/HAVING expression behavior, selected `IN` / `NOT IN` predicate behavior, selected subquery, update-ignore, `REPLACE`, `RETURNING`, temporary create-or-replace, primary-key lookup, selected DDL/name and comment metadata behavior, selected table-close lifecycle under `RENAME TABLE` / `FLUSH TABLES`, selected MTR-profile view, trigger, and stored-procedure DDL/runtime behavior, selected DDL constraint/index metadata and UCA CTAS behavior, selected Aria ALTER/index-upgrade behavior, selected lock-table DDL behavior, selected deprecated server syntax rejection, selected embedded-profile native-engine absence, selected embedded-profile disabled diagnostic behavior, selected embedded-profile disabled metadata behavior, selected embedded-profile host-file SQL I/O rejection, selected embedded-profile optional SQL function absence, selected embedded-profile dynamic-column disabled fallback behavior, selected embedded-profile disabled SQL-surface behavior, selected `ORDER BY` optimizer and aggregate-ordering behavior, optimizer-cost metadata, selected EXPLAIN plan output, selected Aria range, semijoin, and rowid-filter optimizer behavior, selected UNION, EXCEPT / EXCEPT ALL, INTERSECT, and mixed set-operation behavior, prepared-statement behavior, selected compound-statement parser diagnostics, selected missing-routine diagnostics, selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior, selected scalar `LAST_VALUE()`, window `FIRST_VALUE()` / `LAST_VALUE()`, percentile/median, and window-function behavior, selected `IF()` / `NULLIF()` conditional expression behavior, selected SET-family scalar-function behavior, selected scalar operator behavior, selected bit/extraction/replacement/regexp scalar-function behavior, selected default-expression and weight-string behavior, selected string/format function and charset-conversion expression behavior, selected crypto/KDF, disabled DES, and JSON equality/normalization behavior, selected aggregate DISTINCT behavior, selected BIT-key, ODKU, and strict HEAP autoincrement behavior, selected date-format behavior, selected ASCII, legacy, UTF-32, filesystem, Latin2, UTF-8 binary/general, UTF-8 UCA 1400, and utf8mb3 general-1400 charset edge behavior, selected charset weight-string, LIKE condition-propagation, multibyte recoding, and charset CREATE/ALTER inheritance and conversion behavior, and selected charset diagnostics, collation-default behavior, UTF-32 `character_set_collations`, and column/context collation behavior under compatibility-harness work, while MTR-scale comparison remains. |
| 6 | Storage engine skeleton | ✅&nbsp;Done | Register a static MyLite storage engine with controlled handler smoke coverage. |
| 7 | File header and empty catalog | ✅&nbsp;Done | Create/open a valid `.mylite` file with a versioned header and empty catalog. |
| 8 | MyLite metadata DDL and discovery | 🟡&nbsp;In&nbsp;progress | Store routed metadata in the catalog and discover it without durable MariaDB sidecars. Direct and prepared schema namespaces with basic schema options, SQL-layer catalog discovery after reopen, directory-free file-backed initial `CREATE DATABASE` / `CREATE SCHEMA`, catalog-backed `CREATE DATABASE` duplicate, `IF NOT EXISTS`, and `OR REPLACE` paths, create/discovery, ordinary `CREATE TABLE IF NOT EXISTS`, `CREATE TABLE ... LIKE` including FK-source no-FK-copy behavior, successful supported `CREATE TABLE ... SELECT` including generated-source projections, FK-constrained targets for the current supported FK subset, and representative duplicate-mode CTAS, representative temporary LIKE/CTAS catalog isolation, shadowing, and temporary OR REPLACE behavior, representative successful plain `CREATE OR REPLACE TABLE` including generated/CHECK replacement metadata, representative successful `CREATE OR REPLACE TABLE ... LIKE` and CTAS replacement, representative failed OR REPLACE rollback including missing-source LIKE/CTAS inputs, representative FK-aware OR REPLACE parent rejection and child replacement cleanup across plain, LIKE, and CTAS forms, `DROP TABLE`, simple and indexed `RENAME TABLE`, representative table-DDL `IF EXISTS` skip semantics, representative failed table-DDL rollback for multi-table DROP/RENAME including child FK metadata, copy `ALTER` rebuilds including catalog-only reopened CHECK drops, column ALTER existence-option skips, generated-column add/modify/drop, representative primary-key ALTER add/drop/re-add, failed ADD UNIQUE rollback, failed strict conversion ALTER rollback, representative default-algorithm reopened column/index/autoincrement ALTER, and explicit online/in-place ALTER rejection, supported keyed rebuilds, supported index DDL including existence-option skips, SQL-level supported index rename including existence-option skips, supported secondary-index ignorability including `IF EXISTS` skips, representative non-CHECK primary/unique constraint DDL including unique-constraint existence-option paths, explicit key-name matrices, and composite unique constraints, basic CHECK and generated-column metadata persistence, named table-level CHECK add/drop ALTER including existence-option skips, representative `SHOW CREATE TABLE` round-trip export/import including ALTER-evolved generated/CHECK/indexed tables and FK parent/child tables, explicit non-table object rejection, explicit partition DDL and partition-management rejection, and the first public direct/prepared FK DDL publication subset for validated `RESTRICT` / `NO ACTION` constraints over explicit or MariaDB-generated supported child keys are implemented; FK metadata persists child/parent definitions through read/list/parent-list/drop/rename coverage, referenced parent unique-key rename metadata updates, handler FK metadata hooks expose FK records through MariaDB information-schema, parent-table metadata checks, `SHOW CREATE TABLE`, FK-aware copy ALTER column/supporting-key checks with handler-owned retained-key validation under MyLite's FK handlerton advertising, copy-ALTER and successful CTAS metadata preservation including generated supporting-key cleanup, failed FK-constrained CTAS target cleanup, SQL-level referenced-parent drop rejection and child-table FK cleanup, supported `DROP FOREIGN KEY` metadata removal, immediate exact-index child/parent row checks including basic and same-row self-references plus prepared FK DML diagnostics with scalar-parameter checks, ordered multi-row child/self-reference FK checks with failed-statement rollback, representative ordered self-reference update/delete checks, representative non-self parent update/delete rollback ordering, representative multi-table parent-first/child-first FK update/delete ordering, representative parent-target multi-table FK action matrices, bounded self-referencing, same-row self-referencing, non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL`, bounded `ON DELETE CASCADE`, bounded acyclic recursive `ON UPDATE CASCADE`, supported action combinations, same-row update action matrices, explicit `SET DEFAULT` FK action rejection, self-referencing truncate, and session `foreign_key_checks=0` row-DML plus parent-truncate bypass; catalog-backed views, triggers, routines, partition metadata/routing, broader exhaustive multi-table FK update/delete matrices, cyclic or full recursive FK action graphs, broader non-CHECK constraint matrices, broader OR REPLACE edge cases, and broader SQL rollback remain. |
| 9 | Sidecar lifecycle gates | ✅&nbsp;Done | Detect known MariaDB durable engine sidecars around metadata DDL, close/reopen, failed-create cleanup, and MyLite-owned storage-routed MTR schemas. |
| 10 | Engine routing policy | ✅&nbsp;Done | Record requested engine vs. effective MyLite engine and route omitted/default metadata where safe; active file-backed sessions resolve `InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, and `HEAP` directly to MyLite when MyLite is enforced, including under `NO_ENGINE_SUBSTITUTION`, while MariaDB bootstrap/system-table engine access remains native; `BLACKHOLE` routes to MyLite metadata with row-discard behavior, `MEMORY` / `HEAP` route to durable metadata with runtime-volatile row semantics, unsupported explicit `ENGINE` table options reject before catalog publication, and opt-in storage-routed MTR smoke covers selected alias DDL/DML routing, requested-engine `SHOW CREATE TABLE` metadata for omitted/default plus explicit MyLite and alias forms, and MyLite-owned schema sidecar absence. |
| 11 | Row and index storage | 🟡&nbsp;In&nbsp;progress | Table insert, plain replacement target row/index/autoincrement reset, CTAS row population including generated-source projections, generated targets, CHECK-constrained targets, FK-constrained targets for the current supported FK subset, duplicate-mode targets, replacement CTAS targets, and representative temporary OR REPLACE CTAS targets, representative temporary CTAS row visibility and shadowing, full-scan, update/delete, truncate, copy rebuild, NULL values, BLOB/TEXT overflow payloads, generated BLOB/TEXT values, autoincrement state including `ALTER TABLE ... AUTO_INCREMENT`, supported primary/unique/secondary index entries including primary-key drop/re-add and failed unique-add rollback, index preservation across table rename, bounded BLOB/TEXT prefix indexes, bounded generated BLOB/TEXT prefix indexes through initial and standalone copy-rebuild DDL, standalone supported index DDL, SQL-level supported index rename, supported secondary-index ignorability metadata and hint rejection, representative primary/unique constraint-backed key DDL including explicit key-name matrices and composite and nullable composite unique constraints, generated FK child supporting keys and generated FK supporting-key cleanup, basic CHECK enforcement including ALTER existence-option skips, representative grouped ODKU CHECK rollback, and failed ADD CHECK rollback over incompatible existing rows, exact-index FK child/parent row checks over retained storage metadata and supported public FK DDL including basic and same-row self-references plus prepared FK DML diagnostics with scalar-parameter checks, retained FK supporting-key validation under MyLite's FK handlerton advertising, referenced parent unique-key rename row-check preservation, ordered multi-row child/self-reference FK checks with failed-statement rollback, representative ordered self-reference update/delete checks, representative non-self parent update/delete rollback ordering, representative multi-table parent-first/child-first FK update/delete ordering, representative parent-target multi-table FK action matrices, bounded self-referencing, same-row self-referencing, non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL`, bounded `ON DELETE CASCADE`, bounded acyclic recursive `ON UPDATE CASCADE`, supported action combinations, same-row update action matrices, session `foreign_key_checks=0` row-check bypass, fixture-backed FK dump-style out-of-order data import under the same bypass, basic generated columns with copy ALTER add/modify/drop, ordinary generated-column secondary/unique index, generated-column unique constraint including composite virtual matrix coverage and failed-add rollback, generated dependent-column drop rollback, representative generated-column DML and expression-error rollback, and generated-index DDL coverage, prepared diagnostics for representative routed CHECK and generated-column execution failures, prepared routed-storage SELECT coverage for full-scan, primary-key, secondary-index, native integer, text, and scalar-parameter reads, representative CHECK/generated deterministic string, NULL-handling, conditional, temporal, and numeric expression matrices, representative CHECK/generated dump-style import, representative `SHOW CREATE TABLE` round-trip export/import including ALTER-evolved generated/CHECK/indexed tables and FK parent/child tables, generated primary-key rejection, first-key compound autoincrement table-local allocation, live index-entry grouped per-prefix autoincrement maximum lookup including stale delete/update filtering and reverse-sort definitions, representative autoincrement offset/increment behavior including multi-row post-explicit allocation, broader offset/increment pair matrix coverage including offset greater than increment, representative integer-width autoincrement overflow boundaries through signed `BIGINT`, explicit `BIGINT UNSIGNED` maximum-value handling with generated read-failed exhaustion, direct/prepared transaction, direct savepoint, failed/ignored generated insert autoincrement gap preservation including durable first-key generated reservation gaps and source-driven failed/ignored `INSERT ... SELECT` reservation growth, explicit high-value duplicate-insert non-consumption, explicit high-value UPDATE autoincrement semantics including prior-success failed-statement preservation across representative FK, duplicate-key, and CHECK failures, representative mixed-row failed-DML autoincrement matrices, and representative `ON DUPLICATE KEY UPDATE` autoincrement reservation, failed duplicate-update reservation preservation, public statement effects, explicit-update advancement for `INSERT ... VALUES` and `INSERT ... SELECT`, including unknown source-row-count reservation growth, and grouped later-in-key per-prefix allocation, `LAST_INSERT_ID(id)`, explicit high-value advancement, source-driven `INSERT ... SELECT`, prepared `VALUES` / `INSERT ... SELECT`, failed duplicate-update rollback to live prefix maxima, source-read, update-expression, generated-expression, and CHECK-constraint error rollback, grouped failed-DML mixed insert/update rollback plus UPDATE IGNORE skips to live prefix maxima, close/reopen persistence, runtime-volatile MEMORY/HEAP autoincrement overflow rejection, explicit unsupported FULLTEXT/SPATIAL/vector/long-unique index rejection, explicit partition DDL and partition-management rejection, unsupported FK-shape and `SET DEFAULT` FK action rejection, lazy index cursor row materialization, direct row-id materialization without per-row full row-state map rebuilds, key-filtered exact/prefix cursors, storage-level exact-entry and exact-entryset lookup, single-pass durable exact-index scans, catalog-backed index root metadata, contiguous raw fixed-width index leaf runs with page-range lookup and append-tail overlay for exact byte-key lookup plus full index reads, batched opportunistic SQL leaf-root publication for current supported fixed-width keys after copy-rebuild DDL, growable multi-page catalog-chain publication, durable catalog free-list reuse for superseded non-active catalog chains, a local direct/prepared performance baseline harness for routed storage, and opt-in benchmark thresholds are implemented; partition-aware row/index maintenance, broader exhaustive multi-table FK update/delete matrices, cyclic or full recursive FK action graphs, storage-level B-tree navigation, broader grouped `ON DUPLICATE KEY UPDATE` trigger, view, and exhaustive expression-error matrices, broader failed-DML autoincrement matrices, exhaustive autoincrement offset/increment integer-width matrices, exhaustive CHECK/generated expression coverage, full dump/export compatibility, full BLOB/TEXT index support, MySQL-style expression-index compatibility, FULLTEXT/SPATIAL/vector access paths, broader row/index compaction and file shrinking, and broader multi-page transactional index maintenance remain. |
| 12 | Copy `ALTER` rebuilds | ✅&nbsp;Done | Table-copy rebuild support works over the current row and supported index lifecycle. |
| 13 | Primary and secondary indexes | ✅&nbsp;Done | Add append-only index-entry pages, ordered handler cursors, duplicate checks, nullable unique-key semantics, and index maintenance for supported insert/update/delete paths. |
| 14 | Transactions and recovery | 🟡&nbsp;In&nbsp;progress | Rollback-journal atomic publication and recovery with restored-tail truncation are implemented for current append-only storage mutations, covered failed file-backed statements restore a statement-start header/catalog checkpoint including direct and prepared row-DML unique-key failures, representative direct/prepared/select-source multi-row `REPLACE` failures, representative generated-column DML and expression-error rollback, failed strict conversion copy ALTER, representative failed OR REPLACE replacement including missing-source LIKE/CTAS inputs and multi-table DROP/RENAME paths including FK metadata, successful table-DDL `IF EXISTS` skips commit mixed missing/existing DROP/RENAME statements once, initial MariaDB statement transaction hooks drive autocommit row-DML checkpoint commit/rollback, transactional engine table flags align MariaDB capability checks with bounded MyLite row-DML rollback, direct/prepared `BEGIN` / `START TRANSACTION` / `COMMIT` / `ROLLBACK`, transaction restart through repeated direct or prepared `BEGIN` / `START TRANSACTION`, supported direct/prepared `START TRANSACTION READ WRITE` / `READ ONLY`, direct and prepared session `SET autocommit=0/1/DEFAULT` forms, prepared single-marker `SET autocommit=?` transitions, mixed ordinary `SET` lists, and duplicate supported assignment lists applied in order with the final value as session state, direct/prepared `SET TRANSACTION` `READ WRITE` / `READ ONLY` and `ISOLATION LEVEL` forms, direct/prepared transaction read-only and isolation variable assignments including prepared single-marker values and duplicate supported assignment lists where the final value wins, direct/prepared session `SET completion_type=NO_CHAIN/0/DEFAULT/CHAIN/1` including prepared single-marker values and duplicate supported assignment lists where the final value wins, direct/prepared `COMMIT` / `ROLLBACK` `AND CHAIN`, `AND NO CHAIN`, and `NO RELEASE` modifiers, direct plus prepared savepoint rollback/release with case-insensitive simple unquoted, backtick-quoted, and ANSI_QUOTES double-quoted names over durable and MEMORY/HEAP volatile rows, and handler-level native MariaDB savepoint hooks for raw embedded routed durable row-DML plus representative MEMORY volatile rows, read-only transaction rejection for direct and prepared durable MyLite storage writes, direct/prepared read-only transaction row DML against tracked temporary tables, and direct/prepared explicit temporary table create/drop inside active transactions support row-DML transactions through nested MyLite checkpoints and volatile temporary storage; active row-DML transaction crash recovery through transaction journals, same-process two-handle transaction-owner read snapshots, cross-process transaction-journal read snapshots, and generated autoincrement gap preservation after direct/prepared transaction rollback, direct savepoint rollback, MEMORY/HEAP volatile statement, transaction, and savepoint rollback, failed/ignored generated inserts including durable first-key generated reservation gaps and source-driven failed/ignored `INSERT ... SELECT` reservation growth, failed `INSERT ... VALUES` and `INSERT ... SELECT` ODKU generated reservation preservation, explicit high-value duplicate-insert non-consumption, mixed generated `INSERT IGNORE` reserved tail gaps, FK-protected multi-row update non-consumption, successful explicit high-value insert/update rollback preservation, prior-success failed-update explicit autoincrement preservation across representative FK, duplicate-key, and CHECK failures, successful explicit high-value `INSERT ... VALUES` and `INSERT ... SELECT` `ON DUPLICATE KEY UPDATE` rollback preservation, and explicit rejection coverage for global autocommit, global transaction-variable, global `completion_type`, direct marker, expression-valued, global/expression parameterized transaction-control `SET`, bound `DEFAULT` / `RELEASE`, duplicate `SET TRANSACTION` characteristic forms, consistent snapshots, release completion, release completion-type defaults, XA, and durable direct or prepared DDL inside active transactions are covered; broader SQL rollback and broader failed-DML autoincrement matrices, WAL/checkpoints, and full storage isolation remain. |
| 15 | Locking and concurrency | 🟡&nbsp;In&nbsp;progress | Advisory primary-file locks reject unsafe cross-process readers, writers, and recovery races, configured busy timeouts wait for cooperating lock conflicts before returning busy, and representative SQL locking surfaces are rejected until real SQL lock semantics exist; SQL lock integration and full concurrent writers remain. |
| 16 | Compatibility harness | 🟡&nbsp;In&nbsp;progress | Group existing public API, storage, recovery, locking, embedded lifecycle, SQL API comparison including direct/prepared/typed prepared expression result sets, sidecar, routed SQL, transaction-control, transaction-hooks, statement-rollback, partition, foreign-key, foreign-key handler metadata, foreign-key create-info metadata, foreign-key DDL publication, CHECK-constraint, generated-column, unsupported-index, and server-surface including binlog/replication, replication/binlog filter assignments, binlog/replication system-variable assignments and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL file-I/O, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, routine debug-code inspection, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, view runtime, stored-program runtime, trigger runtime, dynamic UDF runtime, and non-table object tests by compatibility surface; an opt-in embedded MTR smoke runner with baseline and storage-routed modes, pass-gated run/probe workflows, probe summaries, reject cleanup, and timeout control, batched support-target build preparation, and suite-batched strict runs covers the trimmed bootstrap schema plus no-default-database connection state, scalar CAST/CONVERT, CASE-family and ANSI SQL-mode expression behavior, selected numeric, hex-hybrid literal/rounding, integer metadata/rounding, character/varchar, binary-string, binary/varbinary, national-character, interval, and type behavior, selected date, temporal-rounding, temporal-function, alternate-timezone conversion, system-timezone behavior, temporal literal behavior, temporal scale, high-resolution temporal function, microsecond parsing, and time/datetime comparison behavior, selected parser/comment, alias, wildcard-alias, keyword, CTE-cycle, precedence, expression, type-coercion, rounding, safe-update, negation-elimination, and comparison behavior, selected boolean aggregate/HAVING expression behavior, selected `IN` / `NOT IN` predicate behavior, selected subquery, update-ignore, `REPLACE`, `RETURNING`, temporary create-or-replace, primary-key lookup, selected DDL/name and comment metadata behavior, selected table-close lifecycle under `RENAME TABLE` / `FLUSH TABLES`, selected MTR-profile view, trigger, and stored-procedure DDL/runtime behavior, selected DDL constraint/index metadata and UCA CTAS behavior, selected Aria ALTER/index-upgrade behavior, selected lock-table DDL behavior, selected deprecated server syntax rejection, selected embedded-profile native-engine absence, selected embedded-profile disabled diagnostic behavior, selected embedded-profile disabled metadata behavior, selected embedded-profile host-file SQL I/O rejection, selected embedded-profile optional SQL function absence, selected embedded-profile dynamic-column disabled fallback behavior, selected embedded-profile disabled SQL-surface behavior, selected storage-routed explicit MyLite and engine-alias DDL/DML behavior, requested-engine `SHOW CREATE TABLE` metadata, explicit MyLite rollback/commit, and MyLite-owned schema sidecar absence, selected `ORDER BY` optimizer and aggregate-ordering behavior, optimizer-cost metadata, selected EXPLAIN plan output, selected Aria range, semijoin, and rowid-filter optimizer behavior, selected UNION, EXCEPT / EXCEPT ALL, INTERSECT, and mixed set-operation behavior, prepared-statement behavior, selected compound-statement parser diagnostics, selected missing-routine diagnostics, selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior, selected scalar `LAST_VALUE()`, window `FIRST_VALUE()` / `LAST_VALUE()`, percentile/median, and window-function behavior, selected `IF()` / `NULLIF()` conditional expression behavior, selected SET-family scalar-function behavior, selected scalar operator behavior, selected bit/extraction/replacement/regexp scalar-function behavior, selected default-expression and weight-string behavior, selected string/format function and charset-conversion expression behavior, selected crypto/KDF, disabled DES, and JSON equality/normalization behavior, selected aggregate DISTINCT behavior, selected BIT-key, ODKU, and strict HEAP autoincrement behavior, selected date-format behavior, selected ASCII, legacy, UTF-32, filesystem, Latin2, UTF-8 binary/general, UTF-8 UCA 1400, and utf8mb3 general-1400 charset edge behavior, selected charset weight-string, LIKE condition-propagation, multibyte recoding, and charset CREATE/ALTER inheritance and conversion behavior, and selected charset diagnostics, collation-default behavior, UTF-32 `character_set_collations`, and column/context collation behavior outside the default groups; MTR-scale comparison and broader application suites remain. |
| 17 | Application schemas | 🟡&nbsp;In&nbsp;progress | Broader WordPress-shaped core-table smoke coverage is implemented for options, posts, postmeta, users, usermeta, terms, taxonomy relationships, comments, commentmeta, and links with representative `utf8mb4_unicode_ci` defaults; WordPress 6.9.4 single-site installer DDL and installer seed fixtures import through omitted-engine routing, including the full deterministic single-site `populate_options()` option-name inventory, selected default values and autoload flags, and the full default role payload; WordPress 6.9.4 multisite global and blog-id-2 schema plus representative network seed fixtures import through omitted-engine routing with indexed network and per-blog rows; BuddyPress 14.4.0 full-component plugin schema and representative rows import through omitted-engine routing with indexed activity, notification, friends, groups, messaging, xProfile, blog-tracking, invitation, and opt-out rows; Laravel v13.6.0 default ORM schema and representative rows import through omitted-engine routing with indexed user, session, cache, job, batch, and failed-job rows; Django 6.0.5 default auth, admin, contenttypes, sessions, and migration-recorder schema plus representative rows import through omitted-engine routing with indexed migration, permission, user, session, and admin-log rows; Rails v8.1.3 Active Record metadata, Active Storage, and Action Text schema plus representative rows import through omitted-engine routing with indexed migration metadata, blob, attachment, variant, and rich-text rows; a representative collation restart/index matrix covers selected utf8mb4, utf8mb3, latin1, latin2, and cp1250 collations including MariaDB 11.8 UCA 1400 defaults; full WordPress runtime install, dynamic PHP/theme/localization installer output, exhaustive collation suites, broader ORM suites and migration runners, Laravel/Django/Rails runtime integration, full multisite runtime, additional per-blog suites, BuddyPress runtime activation, WordPress `dbDelta()` plugin execution, and additional plugin schemas remain. |
| 18 | Server-surface policy | 🟡&nbsp;In&nbsp;progress | Runtime defaults disable networking, grants, binlog, events, and host plugin discovery; representative server SQL plus replication/binlog filter assignment, binlog/replication system-variable assignment and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL host-file import/export, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection including known external no-equals engine names, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, routine debug-code inspection, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, dynamic UDF DDL, and non-table object rejection smoke is implemented; the default profile compiles out dynamic plugin loading, Performance Schema, socket auth, feedback, thread-pool info, userstat, host-file SQL I/O, unsupported server utility functions, the Oracle SQL mode parser, XML SQL functions, GIS SQL functions, vector SQL and MHNSW runtime, SFORMAT, JSON schema validation, JSON table-function execution, dynamic-column packed BLOB runtime, and the full SQL HANDLER, SQL sequence runtime, virtual SEQUENCE storage engine, SQL HELP, PROCEDURE ANALYSE, generic SELECT procedure, view runtime and sidecar metadata, stored-program runtime, event parse-data validation, trigger runtime and sidecar metadata, dynamic UDF runtime, binlog transaction/event-root, log-event server, replication GTID-state runtime, replication filter runtime, binlog/replication system-variable registration for disabled topology features, MyISAM maintenance, native CSV, InnoDB, MyISAM, MRG_MyISAM, and partition engine registration, native-MyISAM-only system variables, foreign-server metadata, external backup SQL, query cache runtime, zlib-backed compression, statement profiling class bodies, optimizer trace runtime, static SHOW information producers, status metadata producers, process-list metadata producers, and routine metadata producers. |
| 19 | Size profile hardening | 🟡&nbsp;In&nbsp;progress | Archive and linked-runtime size reporting is implemented, server-surface trims are measured, and the embedded profile now omits unsupported `LOAD DATA` / `LOAD XML` execution, SQL host-file I/O bodies, low-value server utility SQL functions, the Oracle SQL mode parser, XML SQL functions, GIS SQL functions, vector SQL and MHNSW runtime, SFORMAT, JSON schema validation, JSON table-function execution, dynamic-column packed BLOB runtime, dynamic plugin loading, and the full SQL HANDLER, SQL sequence runtime, virtual SEQUENCE storage engine, SQL HELP, PROCEDURE ANALYSE, generic SELECT procedure, view runtime and sidecar metadata, stored-program runtime and sidecar metadata, event parse-data validation, trigger runtime and sidecar metadata, dynamic UDF runtime, binlog transaction/event-root core, log-event server runtime, replication GTID-state runtime, replication filter runtime, binlog/replication system-variable registration for disabled topology features, MyISAM maintenance, native CSV, InnoDB, MyISAM, MRG_MyISAM, and partition engine registration, native-MyISAM-only system variables, user-statistics plugin, foreign-server metadata, external backup SQL runtime, query cache runtime, zlib-backed compression, statement profiling class bodies, optimizer trace runtime, static SHOW information producers, status metadata producers, process-list metadata producers, and routine metadata producers, including `gtid_index.cc`, `log_event.cc`, `log_event_server.cc`, `rpl_gtid.cc`, `rpl_filter.cc`, `rpl_injector.cc`, `rpl_record.cc`, `mi_check.c`, `mi_keycache.c`, `mi_preload.c`, `ha_innodb.cc`, `handler0alter.cc`, `srv0start.cc`, `trx0trx.cc`, `row0mysql.cc`, `dict0dict.cc`, `buf0buf.cc`, `fil0fil.cc`, `fsp0fsp.cc`, `log0log.cc`, `ha_myisam.cc`, `mi_open.c`, `ft_myisam.c`, `ha_myisammrg.cc`, `myrg_open.c`, `ha_tina.cc`, `transparent_file.cc`, `ha_partition.cc`, `item_vectorfunc.cc`, `vector_mhnsw.cc`, `json_schema.cc`, `json_table.cc`, `ma_dyncol.c`, `sql_handler.cc`, `sql_sequence.cc`, `ha_sequence.cc`, `sequence.cc`, `sql_view.cc`, `event_parse_data.cc`, `sql_trigger.cc`, `userstat.cc`, `sql_servers.cc`, `backup.cc`, `sql_cache.cc`, `emb_qcache.cc`, `opt_trace.cc`, `authors.h`, and `contributors.h` in the disabled profile, with retained `sql_embedded` C++ sources compiled without exceptions, linked first-party embedded smoke binaries no longer linking libz, and generated embedded configs clearing the host dynamic-loader probes; deeper daemon-only and low-value optional component trims remain. |

The opt-in MTR smoke runner also covers selected ODBC compatibility syntax,
optimizer-trace default metadata, SHOW row-order, system `mysql` table
reference, long-tmpdir view, and deprecated rename-database diagnostics while
keeping MTR-scale comparison planned. The storage-routed MTR smoke runner also
covers selected explicit MyLite and engine-alias DDL/DML routing,
requested-engine `SHOW CREATE TABLE` metadata, sidecar absence, explicit
MyLite rollback/commit and DML statement effects, and routed `InnoDB`
rollback, commit, savepoint, and representative foreign-key behavior against a
static MyLite storage-engine build with a primary `.mylite` file, including
explicit MyLite foreign-key publication and enforcement. It
also covers representative routed `InnoDB` and explicit MyLite CHECK and
generated-column metadata, enforcement, generated values, generated-index
reads, and generated unique-key diagnostics through the same raw embedded
storage path, plus representative routed `InnoDB` and explicit MyLite DDL
lifecycle behavior for LIKE, CTAS, copy `ALTER`, indexed reads after rebuild,
and `RENAME TABLE`, plus representative
routed `InnoDB` DML statement effects for ODKU, `REPLACE`, keyed `UPDATE`,
keyed `DELETE`, affected rows, insert ids, and final indexed visibility, plus
representative routed `InnoDB` and explicit MyLite autoincrement generated ids,
explicit high-value advancement, rollback gaps, offset/increment values, and
truncate reset, plus representative unsupported engine request rejection and
failed table metadata absence, plus representative unsupported FULLTEXT/SPATIAL
index rejection.
The baseline MTR smoke list now also includes `main.1st` for the trimmed
embedded-profile bootstrap schema, `main.type_float` for selected
floating-point conversion, comparison, rounding, and metadata inference
coverage, `main.null` for selected NULL/NULLIF expression and metadata
coverage, `main.null_key` for selected indexed NULL-key, `ref_or_null`, and
`IS NULL` optimizer coverage, `main.alias` for selected alias parser,
wildcard-alias, CTAS generated-name, stored-program alias, and column-alias
metadata coverage, and `main.statistics_close` for selected concurrent
`RENAME TABLE` / `FLUSH TABLES` table-close lifecycle coverage under the
embedded profile. It also includes `main.connect-no-db` for selected
no-default-database connection state coverage.
It also includes selected baseline FULLTEXT declaration and Aria FULLTEXT
search coverage while MyLite routed storage continues to reject unsupported
FULLTEXT index definitions explicitly.

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
pages. Single-page maintained roots now cover in-place insert, update, delete,
rollback, recovery, and flagged overflow append-tail visibility for rows
inserted after the root reaches capacity. Contiguous raw fixed-width leaf runs
now serve exact byte-key base snapshots with page-range lookup and append-tail
visibility overlay, including full durable index reads that can start from the
published run instead of scanning the whole append history. SQL copy-rebuild DDL now
opportunistically publishes those roots for all current supported fixed-width
keys in rebuilt tables, including retained primary keys after forced copy
rebuilds, with one shared append-history scan and one catalog publication for
the raw key set. Active checkpoint
write amortization now reuses one statement recovery journal, defers header
publication to checkpoint boundaries, and caches guarded exact duplicate-key
probes on the outer active checkpoint across nested libmylite statement
checkpoints. This materially improves fixed-width index publication
and routed indexed insert throughput. Active checkpoint and snapshot header
reads now reuse the decoded in-memory header instead of re-encoding and
re-checksumming page `0`; nested write checkpoints now clone the parent
header/catalog snapshot instead of revalidating the same active pages for every
prepared row-DML savepoint. Active statements now reuse validated catalog root
images until catalog writes or catalog-generation header changes invalidate the
statement chain. Active row mutation publication now updates decoded checkpoint
headers directly instead of encode/decode round-tripping page `0`.
Durable handler index cursor builds now keep one scoped read session open
across exact-index lookup and row materialization, reusing the validated
primary-file header and catalog image for point lookups and exact-index cursor
builds. Durable exact and full entryset reads now use the same scoped
file/header setup as point lookups and FK prefix probes before ordered cursor
construction falls through to published roots or append-history scans.
Published leaf-root readers now cache resolved index-root catalog entries on
the active statement, avoiding repeated catalog-record scans for the same table
and index root. Repeated read statements over unchanged durable header bytes
and the same file identity also reuse a thread-local decoded checkpoint
snapshot after raw header-page comparison, avoiding repeated header/catalog
checksum validation and redundant catalog-chain reads on hot point-select loops.
Full-row scans, exact row counts, direct row reads, and indexed-row batch
materialization now use the same scoped file/header setup before live-row and
row-payload cache checks.
Schema/table discovery reads, schema/table listing, and foreign-key listing now
use the same scoped setup before catalog-image reads.
Foreign-key definition reads now use the same scoped setup before reading FK
metadata BLOB pages.
Header validation, index-root metadata, and durable autoincrement metadata reads
now use the same scoped setup before scalar metadata resolution.
Residual exact point lookups and broad index-prefix existence scans now close or
execute through the same scoped file lifecycle as the newer index read paths.
Schema namespace catalog writes now use scoped update file/header setup before
publishing catalog mutations, including the no-op existing-schema path.
Index-root catalog writes now use the same scoped update setup before publishing
or removing root metadata records.
Foreign-key metadata writes now use scoped update setup before publishing,
renaming, or dropping FK catalog records and metadata BLOB pages.
Durable autoincrement set and advance writes now use scoped update setup before
resolving table metadata and publishing scalar autoincrement pages.
Table catalog writers now use scoped update setup before publishing table
definition BLOB pages, FK-aware drops, or table rename metadata.
Index leaf rebuild publication now uses scoped update setup before writing
rebuilt leaf pages and catalog root records.
Row append, delete, and truncate mutation paths now use scoped update setup
before writing row pages, row-state pages, maintained index roots, and truncate
autoincrement reset pages; the generic update-open wrapper has been removed.
Normal read statements now reuse a thread-local unlocked read file handle after
device/inode validation, reducing repeated `fopen()` overhead without holding
shared locks between cursor builds. That read handle now stores its device and
inode when cached, avoiding repeated `fstat()` during reuse, and read startup
reuses deterministic journal path strings while still checking both journal
paths each time. Short-lived read statements now also retain one cleaned
statement object per thread, avoiding repeated large statement allocation and
cleanup on point-read loops while leaving recovery probes, shared locks, and
checkpoint header reads unchanged. When a trusted filename identity scope is
active, short-lived read statements also borrow that stable filename pointer
instead of allocating an identical copy; unscoped reusable read statements now
retain their owned filename for same-file reuse while still replacing it across
different files. Durable exact-index and row-payload caches now use that same
trusted filename identity before falling back to filename value comparisons on
hot point-read cache lookups. Direct exact-index and indexed-row payload reads
now also use the active read statement as their table-entry, exact-index, and
row-payload cache owner when a prepared read scope is active. Indexed-row
payload reads now seed row-payload caches through that resolved statement owner
and skip durable row-payload cache probes while an active statement owns the
current file view. Fixed-size random page reads and writes use offset-addressed
`pread()` / `pwrite()` calls, avoiding per-page stdio seek and refill overhead
while leaving sequential journal writes on the stream path.
MariaDB table-discovery callbacks now use the same scoped read sessions for
catalog table-definition, table-list, and existence reads, reducing repeated
prepared statement table-open validation before cursor execution.
File-backed `libmylite` sessions now default `use_stat_tables=NEVER`, avoiding
repeated MariaDB persistent `mysql.*_stats` table discovery for ordinary query
planning until MyLite has file-owned statistics.
The single-table update prepare path now also skips entering the persistent
statistics reader when that session mode is `NEVER`, removing a redundant
disabled-statistics frame from prepared update execution.
Direct and prepared SQL entry points now summarize token-wide unsupported
surface checks through one shared dispatch helper while preserving the existing
command-specific diagnostic priority, reducing repeated MyLite policy preflight
work before MariaDB execution.
Fixed-record durable exact unique-key cursor construction now resolves the
index key and materializes the row payload through one storage operation,
avoiding a second open/header/catalog pass for primary-key point reads. Those
single-entry exact unique cursors now also keep small key, entry, and row-offset
metadata inline in the handler, avoiding per-lookup heap allocation on the
primary-key point-read path. Direct exact unique reads that already copied the
single matching row into MariaDB's record buffer now publish only minimal
one-row continuation state instead of initializing unused cursor key and entry
storage, and skip BLOB payload-slot cleanup because the direct path is limited
to fixed-row tables without BLOB fields. Direct-update exact unique reads now
skip even that one-row continuation state because the update path needs only the
matched row id and row image, while ordinary indexed reads still publish cursor
state for continuation. MariaDB const-table reads through
`index_read_idx_map()` now use the same guarded direct exact-unique
materialization path for supported full-key lookups. Exact indexed row lookup
now borrows the active read statement's decoded catalog view when available,
avoiding a transient catalog copy before table and index-root scans. Active
statement indexed-row reads now also use the statement's current header and
cache chain directly, leaving the generic file-scope/header helper to raw
filename callers. Handler row-update mutations now also reuse the resolved
active statement or matching THD checkpoint, avoiding the generic update-file
scope and header helper on the prepared direct-update mutation path while raw
filename callers keep the scoped update lifecycle. Fixed-record point
reads also reuse a
handler-owned serialized-row scratch
buffer for fixed-record point lookups, and reuse active table-entry metadata
for exact cursor builds and row-write table-id resolution while the catalog
root and generation are unchanged. Active statement, table-entry, and
row-payload cache lookups now inline their hottest resolution helpers and
short-circuit identical string identities before falling back to value
comparison. Active and durable row-lifecycle cache sets now also keep
revalidated last-hit indexes for repeated live-row, live-row-id, and row-payload
lookups. Per-index change predicates and exact-cache bucket resolution now
inline on the row-DML hot path. Active row-payload cache replacements now also
reuse the bucket resolved during old-row validation, avoiding redundant old-row
hash probes while retargeting hot update payloads. Durable row, row-state, and
append-only index-entry page decoders now validate their existing used-byte
checksum shape without scanning unused fixed-page tails.
Cached active update rewrites with one changed index entry now bypass the
generic changed-index page-ref array and second changed-index loop after the
existing buffered-shape cache has validated the row/index layout, and that
fast path trusts the handler-provided changed-key proof instead of comparing the
same serialized key bytes again.
Accepted direct updates now prepare key images only for indexes marked
may-change by MariaDB's direct-update write set, and duplicate checks plus
exact-index cache maintenance handle those sparse entry lists without changing
insert or non-direct update preparation. Additive table and schema metadata
catalog publications now retarget durable table-local caches for unaffected
existing tables instead of stranding them on the prior header identity. Active
exact-index caches can now be created after a prior mutation in the same
transaction without seeding from stale durable caches, and sparse changed-entry
updates retarget omitted unchanged indexes by row id instead of clearing the
active exact cache. Accepted direct updates now also skip empty CHECK,
hidden-index, and in-server helper guard calls after direct-update
initialization has proven the ordinary table shape. MyLite's single-table
update prepare path now also skips value-list `setup_fields()` when every
assigned value is an evaluable basic constant, keeping expression updates on
the normal setup path. The local expression-based prepared-update execute
component still holds near 1.65-1.75 us/op in the 10000-row /
1000000-iteration samples; broader prepared update work can move to remaining
MariaDB table-open, value-expression setup, JOIN prepare, handler, and
checkpoint overhead. The performance harness now keeps ordinary positional
runs capped while adding an explicit `--profile-iterations` option for longer
local sampling runs, so follow-up hot-path work can separate steady execution
frames from short-run setup and cold-cache noise.
Same-size row-only active rewrites now capture only the row checksum-plus-payload
undo range and rewrite only payload bytes while preserving rollback correctness
after dirty buffered-page checksum refreshes and later size-changing rewrites in
the same savepoint. Row-only rewrites now also reuse the buffered row/state
page references resolved by the rewrite dispatcher instead of probing the
append buffer again for those pages, resolve the cached row-only shape once in
the dispatcher, and skip append-replacement cache
bookkeeping when the active rewrite keeps the original row id. A follow-up
profile kept the same-row live-row and exact-index skips while removing a
deferred durable-cache retarget matcher that cost more than the marker rewrite
it avoided. Deferred cache retarget markers now store only the durable cache
header fingerprint fields instead of copying a full storage header on every
row mutation.
Cached one-index active rewrites now do the same for unchanged-size row payloads
and index keys, while size-changing rewrites continue through the conservative
prefix-undo path and upgrade any earlier compact undo entries.
Fixed-width prepared result statements now reuse their result bindings across
reset/re-execute loops and avoid freeing already-drained results a second time,
which exposed parameter binding/reset semantics as the next prepared-path
bottleneck.
Fixed-width prepared result fetches now also skip full per-column current-row
clearing before MariaDB overwrites the reusable result binds and publish fetched
row state with one post-fetch column loop.
Prepared parameter bindings now follow SQLite-style reset semantics: reset keeps
bindings for reuse, clear-bindings releases them explicitly, and repeated
same-type scalar binds avoid redundant MariaDB `mysql_stmt_bind_param()` calls
and object-wide binding resets.
Repeated transient text/blob binds now also reuse stable owned buffers without
rebinding MariaDB parameters when the last bound buffer remains valid.
Successful non-result prepared resets also avoid a redundant MariaDB statement
reset before re-execution. Fully drained result-producing prepared statements
now do the same, while failed statements keep the full MariaDB reset path. The
fully drained `MYSQL_NO_DATA` path also avoids `mysql_stmt_free_result()` for
MyLite's unbuffered prepared result loop. Reset-before-drain for fixed-width
one-row cache candidates now probes one additional row; if it reaches `DONE`,
it publishes the cache entry and keeps the read scope instead of freeing the
active result. Successful non-result prepared execution now also skips the
redundant post-DML `mysql_stmt_free_result()` call because the next MariaDB
execute resets stored-result state before running. Prepared non-result
execution now also reuses the
immutable
SQL policy classified at prepare time, so ordinary prepared DML no longer
reparses its SQL text for no-op transaction or temporary-table lifecycle
updates after every successful step. Ordinary result-producing prepared
statements that do not need MyLite lifecycle, transaction-control, or checkpoint
machinery now execute through a smaller first-step fast path before binding
results and fetching the first row. Clean prepared result loops now skip
warning-vector clearing and current-row cleanup when the database handle and
statement are already in that clean state. The local performance baseline now
includes a prepared scalar-result phase that avoids routed storage, providing a
lower bound for prepared result overhead when judging point-read storage/pager work.
It also includes storage-level primary-key entry and row lookup phases that use
stored primary-key bytes with the MyLite storage API directly, separating raw
storage read-scope, row-materialization, and MariaDB prepared execution cost.
Held-read-scope variants isolate steady-state exact-index and row materialization
cost once one storage read statement is already open, while a storage
read-statement phase measures begin/end overhead directly. A storage row-update
phase measures direct MyLite row mutation under explicit statement checkpoints
without MariaDB prepared execution overhead, and a component variant splits
that direct path into nested checkpoint begin, mutation, and commit timings.
A changed-index storage update component phase now measures the direct
`mylite_storage_update_row_with_index_entry_changes()` shape used by prepared
updates that keep the primary key stable and change a secondary key. A prepared
primary-key component phase now splits the hot prepared loop into bind, row,
done, and reset timings for targeted follow-up work.
A prepared update component phase now splits the hot prepared row-DML loop into
bind, execute, and reset timings before the next write-path optimization pass.
Prepared primary-key miss components now also have a focused benchmark phase,
so repeated no-row point reads can be measured separately from matching row
materialization.
Simple one-parameter prepared point reads can now reuse a bounded one-row
result cache while their retained read statement remains open; MariaDB still
produces the first row, and writes close the read scope before cached rows can
survive storage changes. That cache is now four-way set-associative so larger
stable point-read working sets avoid direct-mapped eviction thrash without
changing cache eligibility or write invalidation. Fixed-width cached row replay
now copies scalar column state in place instead of assigning whole
`ColumnValue` vectors on each cache hit, and cached-row replay plus cached
`DONE` steps now bypass storage-context setup. The prepared point-read cache
now uses a larger bounded set count so 10k-row recurring point-read working sets
fall back to MariaDB less often. It also caches deterministic no-row outcomes
under the same retained read scope, so repeated prepared point misses can return
`DONE` without re-entering MariaDB until a write invalidates the scope. The same
prepared point-read cache now accepts bounded exact text/blob parameter keys,
covering repeated secondary-key-style reads without broadening collation
equivalence beyond identical bound bytes; bound-`NULL` equality misses use the
same no-row cache path. The performance baseline now includes a focused
prepared text-key component phase, so bounded text-parameter cache hits can be
measured separately from integer primary-key hits and no-row misses.
Hot read-checkpoint cache hits now borrow the cached catalog image for scoped
catalog views and defer catalog/header page copies until a caller needs page
bytes, instead of deep-copying that state into every short-lived read statement.
Reusable short read statements now also use a read-specific scalar reset after
their owned resources are released, avoiding the generic reusable statement
initializer on the hot close path while retaining same-file owned filename
reuse.
Successful durable row updates now seed the active transaction row-payload
cache when they entered without an existing payload entry, reducing the local
direct storage row-update mutation component from about 189 us/op to about
7.6 us/op.
Short-lived read statements now promote exact-index and live-row-id caches into
the durable thread-local cache on successful top-level close, avoiding repeated
complete exact-index cache rebuilds across direct point-select loops after
unrelated append history has accumulated.
Recent local storage-smoke benchmark samples show read-statement begin/end at
3.519 us/op, storage primary-key row lookups in the 4-5 us/op range, and routed
prepared primary-key point selects at 7.218 us/op on the current machine after
read-statement object reuse, scoped filename borrowing, clean prepared-state
bookkeeping, lean reusable read-statement reset, and SELECT explain-detail
gating.
Reusable file-backed prepared reads now retain one connection-owned storage read
scope across reset/re-execute loops and close it before connection-local writes,
reducing the local routed prepared primary-key point-select sample to about
2.7 us/op. Fixed-width one-row reset-before-drain now reuses that same retained
scope after a bounded `DONE` probe, reducing the local reset-after-row sample
from about 5015 us/op to about 1.1 us/op.
Handler-local read scopes now detect that retained outer read scope and skip
creating redundant nested read statements, reducing the same local point-select
sample to about 2.3 us/op without broadening handler read-lock lifetime.
The durable row-payload cache now retains a larger bounded hot set, reducing
steady-state 10000-row storage row materialization and routed prepared
primary-key point-select time for the local benchmark. Durable row-payload
cache hits now trust the resolved cache identity and no longer rehash cached
row bytes before copy-out.
Hot successful prepared binds, resets, and
execution entry also skip redundant OK diagnostic string assignment when the
database handle is already in the OK state. Single-part non-null unique-key `UPDATE`
predicates now build
their exact range quick path directly during execution, avoiding the full
range-optimizer rebuild for hot prepared primary-key update loops while leaving
the original `WHERE` condition in place for MariaDB expression semantics.
That exact update quick path now executes through a single exact key-read quick
object instead of generic one-range MRR quick machinery, avoiding dynamic range
allocation and `multi_range_read_next()` overhead for the accepted prepared
point-update shape.
Stable-key prepared point updates now use MyLite's handler direct-update hook
for exact unique-key predicates, reusing MariaDB condition and assignment
evaluation while skipping the normal quick row-read loop. The direct path falls
back for unique-key-changing updates and FK-sensitive key changes, and keeps
non-unique secondary-index maintenance on the existing handler update path.
That direct path now writes accepted changed rows through a MyLite-owned inner
row sequence instead of nesting `handler::ha_update_row()`, while retaining
hidden-index maintenance, handler update statistics, and fallback for
constraint-sensitive table shapes that need MariaDB's private in-server checks.
The handler-side direct key builder now reuses a table-lifetime cloned key
field and key buffer for the accepted one-part integer raw-key subset, avoiding
per-execute `Field::new_key_field()` allocation in MyLite's direct hook while
keeping MariaDB item-to-field conversion semantics.
The SQL-layer exact-update quick shortcut now defers key materialization until
a quick reader is actually reset, and handler-direct updates skip the unused
quick reset/read-record setup, so accepted direct updates no longer build a
second unused lookup key in `SQL_SELECT::check_quick()`.
No-order unique-key `UPDATE` planning now skips the full
`TABLE::update_const_key_parts()` condition walk once the unique quick path has
proved a single candidate row, while still clearing stale const-key state for
the statement.
Accepted MyLite direct updates now keep the SQL-layer exact unique-key proof as
a lightweight marker and materialize `QUICK_EXACT_KEY_SELECT` only if execution
falls back to SQL-layer row discovery, avoiding an unused quick object
allocation on the hot prepared point-update path.
The accepted direct-update path now hands that SQL-layer proof to the MyLite
handler through `info_push()`, so the handler can skip rediscovering the same
exact-key predicate while still evaluating the original `WHERE` condition
before applying the row update.
The handler direct-update row read now trusts that accepted exact-key proof for
its private target-row lookup, leaving generic index reads on the defensive
key-support and raw-exact eligibility path.
Prepared MyLite exact-key updates now keep that reusable SQL-layer proof on
`Sql_cmd_update`, letting each per-execute `SQL_SELECT` borrow it so hot
prepared loops skip condition-tree proof rediscovery while still rerunning NULL
parameter handling and handler acceptance checks.
MyLite handlers now cache direct-update key-shape support at open time, so
accepted prepared exact-key updates do not recompute raw exact-filter support
during every `info_push()`.
Accepted direct-update target-row reads now use a scoped active-statement
indexed-row lookup when a handler or storage-context checkpoint is already
open, avoiding the generic file-scope open/close wrapper in the prepared update
hot loop.
Ordinary MyLite `UPDATE` / `DELETE` execution now skips eager quick-plan
explain detail allocation unless explicit `EXPLAIN`, `ANALYZE`, or slow-log
explain/engine detail needs it, while routed `EXPLAIN UPDATE` keeps the full
plan-detail path.
Accepted MyLite direct updates now also skip the remaining `Explain_update`
node allocation when no explain, analyze, or slow-log detail can observe it,
while non-direct updates keep the node for scan, buffer, and filesort trackers.
They also skip the outer `Explain_query` object created during `JOIN::prepare()`
on the accepted no-explain direct path, while lazily creating it before any
fallback or explain-observable update path needs normal trackers.
Ordinary MyLite single-table base-table UPDATE prepare now elides the
multi-table `multi_update` result helper when `update_single_table()` owns
execution and statement-effect reporting, while broader update shapes keep the
existing helper.
Accepted MyLite direct updates now compute MariaDB row-comparison readiness
once during handler direct-update initialization and reuse that decision during
row mutation, matching the normal update loop's per-statement readiness cache
while preserving unchanged-row affected-count behavior.
They also reuse the cached direct-update key-change mask to skip the redundant
preserve-all index-entry probe on accepted indexed-key updates, while still
computing the changed-entry bitmap from old/new key bytes before storage
mutation.
Accepted MyLite direct updates also skip handler duplicate-key probes during
their inner row update, relying on the direct-update init gate that rejects
unique-key-changing assignments and keeping the probes when an `ON UPDATE`
default could still alter a unique key part.
They now also cache the accepted statement's per-index key-change mask at
direct-update initialization and reuse it during row mutation, avoiding
repeated key metadata walks for indexes that cannot change.
Stable non-key direct-update shapes now also consult that handler shape cache
before repeating the updated-field key-safety and FK-gate admission work, while
key-changing direct updates stay on the uncached gate path.
Pure exact-key direct-update predicates now reuse the accepted key proof for
the final row condition check, while broader `AND` predicates keep evaluating
the full `WHERE` expression after the exact row read.
Ordinary MyLite `SELECT` execution now applies the same explain-detail gate to
table-access plan fields while keeping MariaDB's runtime trackers initialized,
and explicit routed `EXPLAIN SELECT` stays on the full plan-detail path.
Stable-key durable handler updates now use MariaDB's write set to skip
new index-entry serialization, duplicate-key checks, and maintained-root
planning when no supported index key part can change, while retargeting active
exact-index caches by row id.
Handler row-DML now caches immutable table row-lifecycle support and the
resolved auto-increment field at open time, avoiding repeated table/index
metadata walks on hot routed insert, update, and delete paths.
Handler index-entry serialization now also reuses that cached support proof for
checked insert and update paths, so hot row-DML does not rescan immutable key
metadata after the handler has accepted the table shape.
Storage index-entry shape validation now uses the same hot-inline treatment as
the other small row-DML guards, preserving API validation without an extra
out-of-line call on indexed insert and update paths.
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
exact-index cache scan per maintained replacement/delete, and same-row active
updates can replace changed cached key bytes in place without remove-plus-append
cache churn.
Complete durable exact-index cache builds now also track the row ids already
represented in the temporary entryset, avoiding an unconditional entryset scan
before each append-history index entry while preserving replacement/delete
visibility.
Active row-DML statements now remember when no durable live-row-id cache exists
for the current table/header view, avoiding repeated negative cache probes in
hot update loops.
Nested row updates now use the transaction-level live-row validation cache and
cache the table row-state map on the first cold validation miss, so committed
savepoint-sized updates do not repeat row-state visibility scans while rolled
back savepoints still clear speculative parent cache entries.
Transient row-state maps now hash hidden source row ids, removing the
per-candidate linear visibility lookup that dominated full scans after many
updates, and full rowset reads now collect live row ids in one file pass before
materializing only surviving row pages. SQLite-like
row-write and point-read performance still requires the planned navigable index
and pager work. The first pager foundation now wraps fixed-page access for
row-page, row-state, autoincrement, BLOB payload, free-list, append-only
index-entry, and index leaf access while preserving existing storage bytes;
versioned rollback journals can now protect a bounded typed page set for future
pager dirty-page preimages, and active dirty-page rollback now captures,
merges, and restores pager-owned existing page preimages across
statement/savepoint rollback. The first dirty existing-page write in an
ordinary active statement now registers that page in the recovery journal before
writing it, while unsafe journal shapes reject rather than silently bypassing
crash recovery. Active statements can now create that immutable recovery journal
from a bounded preplanned dirty-page set, preparing row/index mutations to
declare candidate maintained pages before row append creates the journal.
Maintained index root pages now have a single-page typed format with
root-owned entry counts, fixed key width, and sorted cells, and small
non-empty fixed-width rebuilds publish that root type while readers dispatch by
root page type. Index-root metadata reads now report maintained root page
counts instead of stale catalog counts. Eligible inserts now update maintained
root pages in place under the preplanned dirty-page journal path and skip the
duplicate append-only index-entry page; rebuild scans preserve entries that
live only in maintained roots. Eligible deletes now physically remove source
row ids from maintained roots while retaining row-state overlays for fallback
visibility. Eligible updates now physically replace maintained root cells for
source-row replacements, preserve root counts, and skip duplicate append-only
replacement index-entry pages when the root was updated in place. Single-page
maintained root insert, update, and delete paths now preserve root bytes and
logical index visibility across statement rollback, transaction rollback,
savepoint rollback, and stale transaction or statement journal recovery. Root
overflow inserts now mark explicit append-tail history on the maintained root,
record the first fallback index-entry page when known, readers scan that tail
only while the flag is set, and root-resident deletes refill the root from live
root-plus-tail entries when they fit again; maintained root mutations now reject
unprotected dirty-root fallback plans. Maintained root insert and update
planning now keeps journal-bounded plan entries and common changed-entry maps
inline, avoiding tiny per-row heap allocations on hot rooted index mutations.
Root
splits, multi-page navigable indexes, and broader transactional maintained
index mutation remain planned.
Durable table-local row,
payload, exact-index, and published leaf-page caches now retarget across
unrelated table row mutations, so one
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
scan when the pre-transaction checkpoint was already cached; those complete
live-row caches keep amortized row-id capacity under their existing entry bound
instead of resizing on every maintained append. The
common inline update path now writes replacement row, row-state, and
replacement index-entry pages as one contiguous append run, reducing per-update
write syscall overhead without changing the durable page format. Inline
inserts now batch the inserted row page and fallback append-only
index-entry pages into one contiguous append run before maintained-root page
updates, reducing per-insert write overhead without changing row ids, page
ordering, or durable bytes. Active
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
and then use a hash-backed validated-row and small page-shape cache for later
rewrites, avoiding repeated row, row-state, and changed index-entry metadata
decodes after the same buffered pages are validated once. Validated row and
changed index-entry pages are now encoded directly in the active append buffer,
removing the temporary read/write page copies from repeated buffered update
rewrites. Those in-buffer rewrites refresh only the row payload or index key
bytes plus any shrunken stale tail instead of rebuilding the whole page image
before recomputing the page checksum. Buffered row and index-entry undo
preimages now copy only the meaningful checksummed prefix and restore an
implicit zero tail, reducing repeated nested-statement rollback bookkeeping.
Active buffered row and changed index-entry rewrites now pass typed prefix
sizes into undo capture, avoiding generic page-type rediscovery before copying
those rollback preimages. Successful row-DML statement cleanup can also retain
one small thread-local buffered-page undo list for later statements, avoiding
repeated small undo-list allocations while statements are inactive. Buffered
page-undo lists now keep transient page-id buckets with 1-based entry indexes,
so larger active update statements skip duplicate undo captures without
scanning the entry array while small undo lists stay on the cheaper linear path
and rollback order remains owned by the append-only entries.
Those repeated buffered rewrites now also leave row and index-entry checksums
dirty until a generic read or buffer flush needs a valid checksum, avoiding
per-update checksum recomputation while pages remain unpublished. The hot
rewrite path now also passes the resolved append-buffer owner through local
helpers instead of rediscovering it from the `FILE *` for each buffered page and
dirty-flag operation. In-place active row rewrites now skip exact-index cache
maintenance for unchanged matching key images and live-row retargeting for an
unchanged row id. Prepared primary-key updates now reuse resolved active
file/cache statement scopes for row validation and active cache maintenance
instead of repeatedly walking active statement chains. Active buffered rewrite
validation now carries append-buffer page refs with checksum-dirty slots through
undo capture and mutation, avoiding repeated page-range and dirty-slot lookup
for already-resolved row and changed index-entry pages. Indexed row lookup now
reuses the resolved active cache statement across exact-index cache probes and
row-payload cache reads before update execution. Prepared update storage now
reuses the write/read statement scope discovered while opening the primary file
for journal start and header publication. Nested write checkpoints now borrow
the parent checkpoint filename instead of allocating an identical filename copy
for every prepared row-DML savepoint. They also use narrow initialization for
nested checkpoint state instead of clearing unused page buffers on every
prepared row-DML savepoint, and retain one cleaned-up nested checkpoint object
per thread for reuse by the next nested statement. Nested checkpoints now also
defer materializing both their current catalog cache and rollback catalog
snapshot until catalog metadata is read, catalog metadata is written, or rollback
needs the statement-start catalog. Hot row-DML and exact-index
lookup paths now pass resolved active cache statements into table-entry cache
helpers instead of rediscovering them by filename. Nested statement cleanup now
retains one small cleared live-row cache set per thread, preserving the active
row validation shortcut while avoiding repeated allocation/free in ordinary
single-row prepared updates. Active row DML now defers durable cache retargeting
to statement commit, so prepared updates in a transaction do not rescan durable
cache sets on every row mutation while rollback still discards pending cache
work. Cached-shape buffered row rewrites with unchanged index entries now skip
replacement state-page and index-page checks after the shape has been validated
once in the active append buffer, while still capturing per-statement rollback
preimages before mutation.
Exact index-entry lookup, indexed-row lookup, and row-update execution now
reuse scoped header state captured while opening the file scope instead of
rediscovering active statement ownership from `FILE *` before reading the
checkpoint header.
Exact-index row-id lookup now defers catalog-image materialization until after
the active exact-index cache misses, avoiding repeated catalog copies on hot
prepared point updates.
Handler exact unique reads and row updates now enter a stable table-name
identity scope, letting active table-entry cache hits bypass repeated name
string comparisons while raw storage callers keep owned-string fallback
matching.
Exact unique reads now also skip creating a handler read-statement scope when
the current THD or storage owner already has an active write checkpoint,
leaving standalone reads on the existing read-scope path.
Row-update maintained-root planning now caches table-level index-root absence
for the active catalog generation, avoiding repeated catalog copies on hot
append-only update loops.
Active row-payload cache reads and replacements now trust active-checkpoint
cache ownership without repeated row-byte checksums.
Same-row active row-payload replacements now update the cached payload entry
after one bucket lookup, leaving bucket remapping to the row-id-changing path.
Scoped exact-index and indexed-row lookup paths also mark statement live-row
caches through the captured active statement instead of rediscovering the same
ownership from `FILE *`.
Validated live-row marking now resolves or appends its statement live-row cache
once before recording both live and validated row ids, removing a duplicate
cache lookup from routed point-update reads.
Indexed-row payload lookup now relies on the successful validated live-row mark
as its single cache update, since that mark already records the row as both
live and validated.
Exact-index cache probes now compare 1/2/4/8-byte key images with fixed-size
loads, keeping `memcmp()` only for larger or variable-width keys.
Those same fixed-width exact-index cache probes now hash 1/2/4/8-byte keys with
integer loads instead of the generic byte loop before bucket lookup.
Active exact-index cache-set and row-id probe helpers are now hot-inlined on
prepared point lookups.
Exact-index cache sets now keep a validated last-hit descriptor for repeated
active and durable table/index/key-size probes.
Prepared point materialization now hot-inlines cached row-payload output helpers
and avoids refreshing an already-current table-name pointer identity.
Active update rewrite probes now hot-inline row-shape, row-id hash, and compact
undo-size helpers used by prepared point-update rewrites.
Repeated table-id resolution now returns through an inlined active table-entry
cache hit and leaves catalog materialization in a cold fallback.
Direct live-row validation now returns through inlined active row-payload and
validated live-row cache hits, leaving storage reads and hidden-row scans in a
cold fallback.
Live-row-id seeding now hot-inlines repeated active statement cache hits and
keeps durable cache promotion in a cold fallback.
Active row-payload replacement now hot-inlines known-entry same-size updates and
leaves cache lookup, resize, removal, and row-id remap cases in a cold fallback.
Cached single-index active rewrites now inline the rewrite and buffered-page
undo pair capture path while preserving rollback preimage copies.
Active exact-index cache maintenance now hot-inlines the unchanged single-cache
case and leaves changed-key, remap, removal, and multi-cache cases in fallback.
Maintained-index-root absence checks now inline repeated statement-cache hits
while preserving catalog planning for uncached or root-backed tables.
Statement journal-owner lookup now inlines the active statement-chain scan used
by prepared update write-journal protection.
Existing transaction/recovery journal owners now skip pager setup and rewrite
checks when a row update has no extra maintained-root protected pages.
Exact unique index cursor construction now reuses the existing row-materialize
eligibility flag instead of scanning table fields again for BLOB/TEXT columns
on prepared point-update reads.
Indexed-row payload lookup and row-update execution now also close borrowed
active-statement file scopes through the captured scope metadata, avoiding
repeated no-op active statement chain rediscovery on prepared point updates.
Those borrowed and cached file handles now clear their stream state through an
unlocked helper on Apple/Linux builds, avoiding stdio lock traffic in the same
hot loop while preserving the existing fallback on other platforms.
Row-update execution now also finishes active-checkpoint write journals through
the captured statement scope, avoiding another filename-based active-statement
lookup on prepared point updates.
Active write-journal begin logic now resolves recovery and transaction journal
owners in one statement-chain pass before protecting dirty pages.
Row-update execution now resolves its active cache owner and same-file append
buffer owner in one parent-chain pass after opening the scoped update file.
Indexed-row payload reads now use the active row-payload cache already resolved
by the caller, avoiding another filename/header/table cache lookup on prepared
point-update reads.
Fixed-width exact-index cache key hash and compare helpers are now hot-inlined
at their cache-probe call sites.
Active row-payload cache probe helpers are now hot-inlined for indexed-row reads
and same-row payload replacement.
Handler index cursor cleanup now skips storage free-wrapper calls for already
empty cursor buffers while preserving inline-buffer ownership checks.
Already-empty handler index cursor cleanup also returns before rewriting cursor
state while preserving logical no-row cursor resets.
Filtered exact and prefix handler index reads now read the first already-filtered
cursor entry directly, avoiding a second key search over the cursor that the
builder just restricted to matching rows.
Small buffered-page undo captures now skip duplicate lookup and bucket ensure
work when the active statement has no prior undo entries and no buckets yet.
Small non-bucketed buffered-page undo lists now also scan duplicate page ids
directly in the capture helper, leaving the bucketed lookup path for larger
statements.
Cached single-index active update rewrites now batch the row and changed-index
undo preimage capture when the nested statement has an empty undo list, keeping
the same rollback order while avoiding duplicated small-list setup.
Durable handler write locks now defer MEMORY/HEAP statement snapshots until a
volatile table participates, while transaction and savepoint snapshots remain
eager at their SQL boundaries.
The libmylite prepared-statement checkpoint wrapper now reuses catalog metadata
to skip statement-level MEMORY/HEAP snapshots for proven durable row-DML targets,
while keeping unknown and volatile targets conservative.
Cached active row-only rewrite shapes now skip the redundant row-state page
range requirement and helper-side shape lookup that were only needed for
uncached shape validation.
Transient row-id cache buckets now use a one-multiply hash with high-bit folding
instead of a heavier two-multiply finalizer.
Native little-endian builds now load fixed-width storage fields and store
32-bit fields with unaligned-safe `memcpy()` fast paths while preserving
byte-loop fallbacks for other targets and 64-bit stores.
Handler write locks now trust an existing THD transaction checkpoint as the
active storage statement proof, avoiding a filename-based storage active-chain
lookup on every row-DML execution inside the same transaction.
Handler exact unique reads, write locks, and statement checkpoint setup now
also trust a non-null libmylite storage context owner with an active statement
before falling back to filename-based storage active-chain lookup.
Handler write locks, exact unique reads, and row updates now also enter a
trusted primary filename identity scope, letting active statement file-scope
lookup avoid repeated filename comparisons while raw storage callers keep
owned-string fallback matching.
Live-row-id cache entries now also remember trusted filename identity pointers
beside their owned filename copies, avoiding durable live-row-id cache filename
comparisons under the handler primary filename scope.
Existing active exact-index cache hits now go through a small hot-inline probe
before falling back to cache creation and seeding, keeping the repeated
prepared point-lookup path out of the miss-handling wrapper.
Non-BLOB handler index cursor rows now validate the fixed record length and
copy directly into the target record buffer, using the opened-handler
BLOB/TEXT metadata cache and leaving descriptor validation on the existing
generic row materialization path.
Reused nested checkpoint objects now skip a second reset when they are taken
back out of the thread-local reusable slot.
Reusable nested checkpoint cleanup now clears only lifecycle flags before
caching the object, leaving full initialization to fresh allocations and parent
snapshot cloning.
Those reusable nested checkpoint objects now also keep bounded buffered-page
undo entry storage attached between prepared row-DML executions, resetting
rollback state without releasing and re-adopting the same small array.
Reusable nested checkpoint cleanup now also skips the general cache/resource
clear path when every resource-owning cache is already empty, reducing hot
prepared row-DML commit cleanup while leaving broader statement cleanup
unchanged.
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
secondary exact reads. Handler cursor materialization now also reuses a
handler-owned row-id scratch buffer instead of allocating a temporary row-id
array for every non-unique cursor batch.
Handler instances now cache proven child and parent foreign-key metadata
absence for opened tables, removing repeated no-op FK catalog scans from
ordinary non-FK row-DML paths, with successful local table DDL invalidating
already-open handler caches through a process-wide FK metadata epoch. Published
leaf-root exact reads now return the first row id directly for static published
runs, and bulk-grow matching entrysets and row-id lists per leaf page, removing
temporary row-id list allocation from static point probes and per-row-id array
reallocations from many-match secondary lookups. Internal row-id result lists
now keep amortized capacity, so append-history exact scans and append-tail
overlays no longer resize the list on every discovered row id.
Leaf-run exact and full-read helpers now reuse the validated root leaf page
decoded during run discovery for offset `0`, avoiding redundant leaf-page cache
lookups on single-page published roots.
Full fixed-size index reads that still fall through to append-history scans now
seed and reuse complete exact-index entryset caches through active read
statements and durable thread-local cache guards. This keeps non-raw text-key
equality under MariaDB comparison semantics while avoiding repeated full storage
scans during prepared text-key cache warm-up.
Durable foreign-key prefix checks now probe complete static published leaf roots
and single-page maintained roots directly, falling back to the materialized
overlay path when append-tail history or missing roots require it. Static
prefix probes lower-bound within each sorted leaf page, including shorter
serialized prefixes for composite-key FK checks, and they now use scoped
file/header reads like exact-index point lookups.
Handler row-DML now prepares small index-entry/key buffers on the stack for
common fixed-width tables, avoiding hot per-row heap churn while larger key
sets still use the existing heap path.
Handler update changed-key detection now uses MariaDB's write set to skip
old-key image reconstruction for indexes whose key-part fields were not
written, while handler-owned same-row foreign-key rewrites mark their locally
mutated key columns before the filter runs.
Nested storage checkpoints now materialize rollback header-page bytes lazily,
removing eager page-0 encoding and checksumming from successful prepared
row-DML statements inside an active transaction.
Storage page integer accessors now use hot inline definitions while keeping the
existing byte-wise little-endian encoding, removing tiny helper call overhead
from hot row/index page update paths without relying on unaligned host loads.
Buffered row-DML now coalesces adjacent active-statement and append-buffer
lookups for hot append/rewrite paths, preserving nearest active statement versus
outermost append-buffer semantics while avoiding duplicate statement-chain scans.
Buffered row and index-entry rewrite helpers now use hot inline definitions,
removing another pair of tiny MyLite-owned leaf calls from the prepared-update
fast path without changing page bytes or checksum-dirty ownership.
Same-size cached one-index buffered rewrites now batch their compact row and
index-entry rollback range captures, preserving the existing prefix fallback
for broader undo-list shapes while avoiding duplicate empty-list setup on the
hot prepared-update path.
Successful active buffered rewrites now skip publishing the unchanged header
and the no-op active write-journal finish, while append and maintained-root
update paths still publish any advanced header.
Active buffered row-update rewrites now defer write-journal setup until after
the rewrite attempt, so steady prepared updates that only mutate buffered pages
avoid the journal-begin check while append and maintained-root update paths
still journal before durable writes.
Prepared row-update execution now threads resolved active live-row and
row-payload cache pointers through validation and post-update cache maintenance,
avoiding repeated cache-set scans and filename/catalog comparisons inside the
same storage update call.
It now also reuses the active row-payload cache entry resolved during live-row
validation when replacing same-row cached payload bytes after update.
Active buffered row-update rewrites now receive the already-resolved active
statement and append-buffer scope from row-update execution, avoiding another
statement-chain walk from `FILE *` on each prepared point update.
Row-only active buffered rewrites now use a dedicated no-index helper after the
row/state shape has been validated, avoiding generic changed-index shape checks
on stable-key prepared updates.
Active buffered row-update rewrites now resolve the contiguous append-buffer
rewrite range once and derive row, row-state, and changed-index page refs from
it, avoiding repeated range and page-ref checks on the prepared update path.
Exact-index row-id lookups now return immediately from completed active and
durable exact-cache hits, leaving leaf-root and append-history fallbacks only
for no-cache paths. Hot exact indexed-row cache-hit paths now also skip
catalog-image cleanup when no local catalog image was allocated. Row-only
storage update cleanup now skips the same empty-catalog call when maintained
index-root planning did not allocate a local catalog image.
Nested statement checkpoints inside active transactions now begin from the
known transaction or savepoint parent, avoiding filename-based active-statement
rediscovery on prepared row-DML loops.
The MyLite handler lock path now threads an already-observed active checkpoint
into statement-checkpoint setup, avoiding a second active-chain lookup during
routed prepared row-DML execution.
Handler row updates now reuse the per-call parent foreign-key presence result,
avoiding a second handler FK-presence guard on each checked update while keeping
child checks and FK action order unchanged.
Active live-row caches now keep hash-backed row-id membership for larger live
and payload-validated row-id sets, while small nested statement caches stay on
the cheaper linear path.
The transient active append-page buffer now uses a 32768-page window, keeping a
10k-row replacement generation resident for repeated update rewrites at the
cost of a 128 MiB worst-case per-checkpoint memory window.
Active row-payload caches now also keep a larger byte-bounded small-row working
set, so repeated indexed updates over 10k rows do not thrash the old 4096-entry
payload-cache window while large payloads remain bounded by a 16 MiB per-table
cache budget.
Rows retained by that active payload cache now also act as the direct
row-validation proof for the same active checkpoint view, reducing duplicate
live-row validation cache work after indexed row reads. Row-update validation
now also reuses the already-resolved active payload cache pointer instead of
rediscovering that cache owner for each mutation.
Prepared embedded MyLite updates now skip formatting the connection-level
MariaDB OK information string on the ordinary no-result statement path, while
preserving affected rows, last insert ids, warning counts, direct text-query
info strings, bulk diagnostics, and system-versioned update messages.
Eligible embedded prepared MyLite OK responses with no information string now
copy scalar status directly to the connection and statement, avoiding the
per-execute `MYSQL_DATA` / `embedded_query_result` allocation on hot no-result
prepared loops while keeping result-set, error, direct text-query, and
multi-result delivery on MariaDB's embedded result queue.
Prepared no-result execution now also skips the cached temporary-table
lifecycle apply helper when the prepared SQL has no lifecycle names, while
still applying cached create/drop effects for prepared temporary-table DDL.
Prepared statement execution now constructs the resolved transaction-control
vector only for parameterized transaction-control statements, avoiding the
empty vector construction/destruction on ordinary prepared row-DML steps.
The MyLite handler now caches the resolved active storage checkpoint for direct
target-row reads and row updates, avoiding repeated rediscovery of the same
active statement from storage thread-local state.
The local performance baseline now accepts opt-in per-metric microsecond
thresholds, giving performance slices a reusable regression gate while keeping
default runs descriptive and machine-local. It also has focused primary-key
point-select phases so read-path slices can measure direct and prepared point
lookups, including SQLite-style prepared reset-after-row lookups, without
running the slower secondary-result phases. It now also has a focused
`prepared-assignment-update-components` phase for `SET value = ? WHERE id = ?`,
separate from the expression-based `prepared-update-components` phase for
`SET value = value + 1 WHERE id = ?`; the simple assignment phase locally
measures about 1.62-1.63 us/op for the step component over 10000 rows /
1000000 iterations. A row-only expression phase,
`prepared-row-only-update-components`, keeps the same prepared exact-key SQL
path but updates a non-indexed integer `counter` column in a separate table,
separating SQL-layer prepared update overhead from secondary-index replacement
cost. A companion `prepared-row-only-update-miss-components` phase binds
out-of-range primary keys against the same row-only table and records a zero
checksum, isolating table-open, prepare, lock, exact lookup, and reset cost
from row materialization and storage mutation; a local 10000-row /
1000000-iteration sample measured the no-match step at about 1.14 us/op.
The next prepared-DML performance wall is repeated MariaDB table-open,
`Sql_cmd_update::prepare_inner()`, and `JOIN::prepare()` work before the
accepted MyLite direct-update path. The staged design is tracked in
[Prepared DML execution reuse](specs/prepared-dml-execution-reuse/specs.md).
The next implementation boundary is
[Prepared direct update rebind](specs/prepared-direct-update-rebind/specs.md):
keep table opening and locking per execution, explicitly rebind the freshly
opened table state, and only then attempt to bypass repeated `JOIN::prepare()`
for the already-proven exact-key MyLite direct-update subset.
The first implementation step caches the immutable prepared-update value-list
subquery shape on `Sql_cmd_update`, avoiding repeated value-list scans before
the MyLite single-update result-elision gate. The next step skips value-list
`setup_fields()` for simple literal and bound-scalar update values while
preserving the normal setup path for expressions and contextual values. The
current expression-update benchmark still shows repeated table-open,
value-expression setup, and `JOIN::prepare()` work as the next prepared-DML
performance wall. The MyLite handler now also caches accepted non-key
direct-update shape facts for prepared statements, avoiding repeated
handler-side metadata walks for stable row-only updates while key-changing
updates keep the existing uncached FK-sensitive path. Accepted integer-key
direct updates now also serialize bound integer lookup keys through a guarded
handler-local `Field::store()` path instead of the generic `Item::save_in_field()`
dispatcher, while non-integer predicate values keep the existing MariaDB
conversion path. Accepted direct updates now hoist the stable filename and
table identity scopes across the exact read plus update mutation, so the inner
storage read and write helpers no longer establish duplicate identical scopes
for the same direct-update operation.

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
