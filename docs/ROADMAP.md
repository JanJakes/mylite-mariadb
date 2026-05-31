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
| 5 | SQL execution API | 🟡&nbsp;In&nbsp;progress | Add direct execution, prepared statements, bindings, columns, warnings, affected rows, and insert ids. Direct and prepared statement-effect coverage including representative `ON DUPLICATE KEY UPDATE` duplicate-update insert-id behavior with `LAST_INSERT_ID(id)`, routed-storage ODKU affected-row and insert-id assertions for `INSERT ... VALUES` and `INSERT ... SELECT`, prepared-statement typed values and parameter counts, column metadata, warning enumeration including selected failed paths, prepared diagnostics for representative routed CHECK, generated-column, and FK execution failures with scalar-parameter FK checks, current-row large-value reads, prepared routed-storage SELECT coverage for aggregate, primary-key, secondary-index, text, native integer, and scalar-parameter reads, explicit direct/prepared file-import and file-export rejection, and representative MariaDB baseline comparison for values, direct/prepared/typed prepared expression result sets, metadata, warnings, and statement effects are implemented; rich prepared parameter metadata is explicitly unsupported on the current MariaDB base, and the opt-in MTR smoke runner now covers no-default-database connection state, scalar CAST/CONVERT, CASE-family and ANSI SQL-mode expression behavior, selected numeric, hex-hybrid literal/rounding, integer metadata/rounding, character/varchar, binary-string, binary/varbinary, national-character, interval, and type behavior, selected date, temporal-rounding, temporal-function, alternate-timezone conversion, leap-second timezone conversion, system-timezone behavior, temporal literal behavior, temporal scale, high-resolution temporal function, microsecond parsing, and time/datetime comparison behavior, selected parser/comment, alias, wildcard-alias, keyword, CTE-cycle, precedence, expression, type-coercion, rounding, safe-update, negation-elimination, and comparison behavior, selected boolean aggregate/HAVING expression behavior, selected `IN` / `NOT IN` predicate behavior, selected subquery, subquery `ANALYZE FORMAT=JSON`, update-ignore, `REPLACE`, `RETURNING`, temporary create-or-replace, primary-key lookup, selected DDL/name and comment metadata behavior, selected table-close lifecycle under `RENAME TABLE` / `FLUSH TABLES`, selected MTR-profile view, trigger, and stored-procedure DDL/runtime behavior, selected DDL constraint/index metadata and UCA CTAS behavior, selected Aria ALTER/index-upgrade behavior, selected lock-table DDL behavior, selected deprecated server syntax rejection, selected embedded-profile native-engine absence, selected embedded-profile disabled diagnostic behavior, selected embedded-profile disabled metadata behavior, selected embedded-profile host-file SQL I/O rejection, selected embedded-profile optional SQL function absence, selected embedded-profile dynamic-column disabled fallback behavior, selected embedded-profile disabled SQL-surface behavior, selected `ORDER BY` optimizer and aggregate-ordering behavior, optimizer-cost metadata, selected EXPLAIN plan output, selected Aria range, semijoin, and rowid-filter optimizer behavior, selected UNION, EXCEPT / EXCEPT ALL, INTERSECT, and mixed set-operation behavior, prepared-statement behavior, selected compound-statement parser diagnostics, selected missing-routine diagnostics, selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior, selected scalar `LAST_VALUE()`, window `FIRST_VALUE()` / `LAST_VALUE()`, percentile/median, and window-function behavior, selected `IF()` / `NULLIF()` conditional expression behavior, selected SET-family scalar-function behavior, selected scalar operator behavior, selected bit/extraction/replacement/regexp scalar-function behavior, selected default-expression and weight-string behavior, selected string/format function and charset-conversion expression behavior, selected crypto/KDF, disabled DES, and JSON equality/normalization behavior, selected aggregate DISTINCT and indexed count-distinct behavior, selected BIT-key, ODKU, and strict HEAP autoincrement behavior, selected date-format behavior, selected ASCII, legacy, UTF-32, filesystem, Latin2, UTF-8 binary/general, UTF-8 UCA 1400, and utf8mb3 general-1400 charset edge behavior, selected charset weight-string, LIKE condition-propagation, multibyte recoding, and charset CREATE/ALTER inheritance and conversion behavior, and selected charset diagnostics, collation-default behavior, UTF-32 `character_set_collations`, column/context collation behavior, selected locale formatting, selected system-variable capability/local-infile/security behavior, and selected support-tool/mysqltest behavior under compatibility-harness work; MTR coverage inventory now reports accepted upstream coverage and known unsupported probes against the imported test-file set, while executable MTR-scale comparison and broader unsupported-surface normalization remain. |
| 6 | Storage engine skeleton | ✅&nbsp;Done | Register a static MyLite storage engine with controlled handler smoke coverage. |
| 7 | File header and empty catalog | ✅&nbsp;Done | Create/open a valid `.mylite` file with a versioned header and empty catalog. |
| 8 | MyLite metadata DDL and discovery | 🟡&nbsp;In&nbsp;progress | Store routed metadata in the catalog and discover it without durable MariaDB sidecars. Direct and prepared schema namespaces with basic schema options, SQL-layer catalog discovery after reopen, directory-free file-backed initial `CREATE DATABASE` / `CREATE SCHEMA`, catalog-backed `CREATE DATABASE` duplicate, `IF NOT EXISTS`, and `OR REPLACE` paths, create/discovery, ordinary `CREATE TABLE IF NOT EXISTS`, `CREATE TABLE ... LIKE` including FK-source no-FK-copy behavior, successful supported `CREATE TABLE ... SELECT` including generated-source projections, FK-constrained targets for the current supported FK subset, and representative duplicate-mode CTAS, representative temporary LIKE/CTAS catalog isolation, shadowing, and temporary OR REPLACE behavior, representative successful plain `CREATE OR REPLACE TABLE` including generated/CHECK replacement metadata, representative successful `CREATE OR REPLACE TABLE ... LIKE` and CTAS replacement, representative failed OR REPLACE rollback including missing-source LIKE/CTAS inputs, representative FK-aware OR REPLACE parent rejection and child replacement cleanup across plain, LIKE, and CTAS forms, `DROP TABLE`, simple and indexed `RENAME TABLE`, representative table-DDL `IF EXISTS` skip semantics, representative failed table-DDL rollback for multi-table DROP/RENAME including child FK metadata, copy `ALTER` rebuilds including catalog-only reopened CHECK drops, column ALTER existence-option skips, generated-column add/modify/drop, representative primary-key ALTER add/drop/re-add, failed ADD UNIQUE rollback, failed strict conversion ALTER rollback, representative default-algorithm reopened column/index/autoincrement ALTER, and explicit online/in-place ALTER rejection, supported keyed rebuilds, supported index DDL including existence-option skips, SQL-level supported index rename including existence-option skips, supported secondary-index ignorability including `IF EXISTS` skips, representative non-CHECK primary/unique constraint DDL including unique-constraint existence-option paths, explicit key-name matrices, and composite unique constraints, basic CHECK and generated-column metadata persistence, named table-level CHECK add/drop ALTER including existence-option skips, representative `SHOW CREATE TABLE` round-trip export/import including ALTER-evolved generated/CHECK/indexed tables and FK parent/child tables, explicit non-table object rejection, explicit partition DDL and partition-management rejection, and the first public direct/prepared FK DDL publication subset for validated `RESTRICT` / `NO ACTION` constraints over explicit or MariaDB-generated supported child keys are implemented; FK metadata persists child/parent definitions through read/list/parent-list/drop/rename coverage, referenced parent unique-key rename metadata updates, handler FK metadata hooks expose FK records through MariaDB information-schema, parent-table metadata checks, `SHOW CREATE TABLE`, FK-aware copy ALTER column/supporting-key checks with handler-owned retained-key validation under MyLite's FK handlerton advertising, copy-ALTER and successful CTAS metadata preservation including generated supporting-key cleanup, failed FK-constrained CTAS target cleanup, SQL-level referenced-parent drop rejection and child-table FK cleanup, supported `DROP FOREIGN KEY` metadata removal, immediate exact-index child/parent row checks including basic and same-row self-references plus prepared FK DML diagnostics with scalar-parameter checks, ordered multi-row child/self-reference FK checks with failed-statement rollback, representative ordered self-reference update/delete checks, representative non-self parent update/delete rollback ordering, representative multi-table parent-first/child-first FK update/delete ordering, representative parent-target multi-table FK action matrices, bounded self-referencing, same-row self-referencing, non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL`, bounded `ON DELETE CASCADE`, bounded acyclic recursive `ON UPDATE CASCADE`, supported action combinations, same-row update action matrices, explicit `SET DEFAULT` FK action rejection, self-referencing truncate, and session `foreign_key_checks=0` row-DML plus parent-truncate bypass; catalog-backed views, triggers, routines, partition metadata/routing, broader exhaustive multi-table FK update/delete matrices, cyclic or full recursive FK action graphs, broader non-CHECK constraint matrices, broader OR REPLACE edge cases, and broader SQL rollback remain. |
| 9 | Sidecar lifecycle gates | ✅&nbsp;Done | Detect known MariaDB durable engine sidecars around metadata DDL, close/reopen, failed-create cleanup, and MyLite-owned storage-routed MTR schemas. |
| 10 | Engine routing policy | ✅&nbsp;Done | Record requested engine vs. effective MyLite engine and route omitted/default metadata where safe; active file-backed sessions resolve `InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, and `HEAP` directly to MyLite when MyLite is enforced, including under `NO_ENGINE_SUBSTITUTION`, while MariaDB bootstrap/system-table engine access remains native; `BLACKHOLE` routes to MyLite metadata with row-discard behavior, `MEMORY` / `HEAP` route to durable metadata with runtime-volatile row semantics, unsupported explicit `ENGINE` table options reject before catalog publication, and opt-in storage-routed MTR smoke covers selected alias DDL/DML routing, requested-engine `SHOW CREATE TABLE` metadata for omitted/default plus explicit MyLite and alias forms, and MyLite-owned schema sidecar absence. |
| 11 | Row and index storage | 🟡&nbsp;In&nbsp;progress | Table insert, plain replacement target row/index/autoincrement reset, CTAS row population including generated-source projections, generated targets, CHECK-constrained targets, FK-constrained targets for the current supported FK subset, duplicate-mode targets, replacement CTAS targets, and representative temporary OR REPLACE CTAS targets, representative temporary CTAS row visibility and shadowing, full-scan, update/delete, truncate, copy rebuild, NULL values, BLOB/TEXT overflow payloads, generated BLOB/TEXT values, autoincrement state including `ALTER TABLE ... AUTO_INCREMENT`, supported primary/unique/secondary index entries including primary-key drop/re-add and failed unique-add rollback, index preservation across table rename, bounded BLOB/TEXT prefix indexes, bounded generated BLOB/TEXT prefix indexes through initial and standalone copy-rebuild DDL, standalone supported index DDL, SQL-level supported index rename, supported secondary-index ignorability metadata and hint rejection, representative primary/unique constraint-backed key DDL including explicit key-name matrices and composite and nullable composite unique constraints, generated FK child supporting keys and generated FK supporting-key cleanup, basic CHECK enforcement including ALTER existence-option skips, representative grouped ODKU CHECK rollback, and failed ADD CHECK rollback over incompatible existing rows, exact-index FK child/parent row checks over retained storage metadata and supported public FK DDL including basic and same-row self-references plus prepared FK DML diagnostics with scalar-parameter checks, retained FK supporting-key validation under MyLite's FK handlerton advertising, referenced parent unique-key rename row-check preservation, ordered multi-row child/self-reference FK checks with failed-statement rollback, representative ordered self-reference update/delete checks, representative non-self parent update/delete rollback ordering, representative multi-table parent-first/child-first FK update/delete ordering, representative parent-target multi-table FK action matrices, bounded self-referencing, same-row self-referencing, non-self `ON DELETE SET NULL` / `ON UPDATE SET NULL`, bounded `ON DELETE CASCADE`, bounded acyclic recursive `ON UPDATE CASCADE`, supported action combinations, same-row update action matrices, session `foreign_key_checks=0` row-check bypass, fixture-backed FK dump-style out-of-order data import under the same bypass, basic generated columns with copy ALTER add/modify/drop, ordinary generated-column secondary/unique index, generated-column unique constraint including composite virtual matrix coverage and failed-add rollback, generated dependent-column drop rollback, representative generated-column DML and expression-error rollback, and generated-index DDL coverage, prepared diagnostics for representative routed CHECK and generated-column execution failures, prepared routed-storage SELECT coverage for full-scan, primary-key, secondary-index, native integer, text, and scalar-parameter reads, representative CHECK/generated deterministic string, NULL-handling, conditional, temporal, and numeric expression matrices, representative CHECK/generated dump-style import, representative `SHOW CREATE TABLE` round-trip export/import including ALTER-evolved generated/CHECK/indexed tables and FK parent/child tables, generated primary-key rejection, first-key compound autoincrement table-local allocation, live index-entry grouped per-prefix autoincrement maximum lookup including stale delete/update filtering and reverse-sort definitions, representative autoincrement offset/increment behavior including multi-row post-explicit allocation, broader offset/increment pair matrix coverage including offset greater than increment, representative integer-width autoincrement overflow boundaries through signed `BIGINT`, explicit `BIGINT UNSIGNED` maximum-value handling with generated read-failed exhaustion, direct/prepared transaction, direct savepoint, failed/ignored generated insert autoincrement gap preservation including durable first-key generated reservation gaps and source-driven failed/ignored `INSERT ... SELECT` reservation growth, explicit high-value duplicate-insert non-consumption, explicit high-value UPDATE autoincrement semantics including prior-success failed-statement preservation across representative FK, duplicate-key, and CHECK failures, representative mixed-row failed-DML autoincrement matrices, and representative `ON DUPLICATE KEY UPDATE` autoincrement reservation, failed duplicate-update reservation preservation, public statement effects, explicit-update advancement for `INSERT ... VALUES` and `INSERT ... SELECT`, including unknown source-row-count reservation growth, and grouped later-in-key per-prefix allocation, `LAST_INSERT_ID(id)`, explicit high-value advancement, source-driven `INSERT ... SELECT`, prepared `VALUES` / `INSERT ... SELECT`, failed duplicate-update rollback to live prefix maxima, source-read, update-expression, generated-expression, and CHECK-constraint error rollback, grouped failed-DML mixed insert/update rollback plus UPDATE IGNORE skips to live prefix maxima, close/reopen persistence, runtime-volatile MEMORY/HEAP autoincrement overflow rejection, explicit unsupported FULLTEXT/SPATIAL/vector/long-unique index rejection, explicit partition DDL and partition-management rejection, unsupported FK-shape and `SET DEFAULT` FK action rejection, lazy index cursor row materialization, direct row-id materialization without per-row full row-state map rebuilds, key-filtered exact/prefix cursors, storage-level exact-entry and exact-entryset lookup, single-pass durable exact-index scans, catalog-backed index root metadata, contiguous raw fixed-width index leaf runs with page-range lookup and append-tail overlay for exact byte-key lookup plus full index reads, batched opportunistic SQL leaf-root publication for current supported fixed-width keys after copy-rebuild DDL, validated branch-page format, single-level branch roots for fitting published leaf runs with exact and prefix lower-bound child selection, stored child-page-id reads, and page-owned branch entry counts, growable multi-page catalog-chain publication, durable catalog free-list reuse for superseded non-active catalog chains, a local direct/prepared performance baseline harness for routed storage, and opt-in benchmark thresholds are implemented; partition-aware row/index maintenance, broader exhaustive multi-table FK update/delete matrices, cyclic or full recursive FK action graphs, production storage-level B-tree split/merge/navigation, broader grouped `ON DUPLICATE KEY UPDATE` trigger, view, and exhaustive expression-error matrices, broader failed-DML autoincrement matrices, exhaustive autoincrement offset/increment integer-width matrices, exhaustive CHECK/generated expression coverage, full dump/export compatibility, full BLOB/TEXT index support, MySQL-style expression-index compatibility, FULLTEXT/SPATIAL/vector access paths, broader row/index compaction and file shrinking, and broader multi-page transactional index maintenance remain. |
| 12 | Copy `ALTER` rebuilds | ✅&nbsp;Done | Table-copy rebuild support works over the current row and supported index lifecycle. |
| 13 | Primary and secondary indexes | ✅&nbsp;Done | Add append-only index-entry pages, ordered handler cursors, duplicate checks, nullable unique-key semantics, and index maintenance for supported insert/update/delete paths. |
| 14 | Transactions and recovery | 🟡&nbsp;In&nbsp;progress | Rollback-journal atomic publication and recovery with restored-tail truncation are implemented for current append-only storage mutations, covered failed file-backed statements restore a statement-start header/catalog checkpoint including direct and prepared row-DML unique-key failures, representative direct/prepared/select-source multi-row `REPLACE` failures, representative generated-column DML and expression-error rollback, failed strict conversion copy ALTER, representative failed OR REPLACE replacement including missing-source LIKE/CTAS inputs and multi-table DROP/RENAME paths including FK metadata, successful table-DDL `IF EXISTS` skips commit mixed missing/existing DROP/RENAME statements once, initial MariaDB statement transaction hooks drive autocommit row-DML checkpoint commit/rollback, transactional engine table flags align MariaDB capability checks with bounded MyLite row-DML rollback, direct/prepared `BEGIN` / `START TRANSACTION` / `COMMIT` / `ROLLBACK`, transaction restart through repeated direct or prepared `BEGIN` / `START TRANSACTION`, supported direct/prepared `START TRANSACTION READ WRITE` / `READ ONLY`, direct and prepared session `SET autocommit=0/1/DEFAULT` forms, prepared single-marker `SET autocommit=?` transitions, mixed ordinary `SET` lists, and duplicate supported assignment lists applied in order with the final value as session state, direct/prepared `SET TRANSACTION` `READ WRITE` / `READ ONLY` and `ISOLATION LEVEL` forms, direct/prepared transaction read-only and isolation variable assignments including prepared single-marker values and duplicate supported assignment lists where the final value wins, direct/prepared session `SET completion_type=NO_CHAIN/0/DEFAULT/CHAIN/1` including prepared single-marker values and duplicate supported assignment lists where the final value wins, direct/prepared `COMMIT` / `ROLLBACK` `AND CHAIN`, `AND NO CHAIN`, and `NO RELEASE` modifiers, direct plus prepared savepoint rollback/release with case-insensitive simple unquoted, backtick-quoted, and ANSI_QUOTES double-quoted names over durable and MEMORY/HEAP volatile rows, and handler-level native MariaDB savepoint hooks for raw embedded routed durable row-DML plus representative MEMORY volatile rows, read-only transaction rejection for direct and prepared durable MyLite storage writes, direct/prepared read-only transaction row DML against tracked temporary tables, and direct/prepared explicit temporary table create/drop inside active transactions support row-DML transactions through nested MyLite checkpoints and volatile temporary storage; active row-DML transaction crash recovery through transaction journals, same-process two-handle transaction-owner read snapshots, cross-process transaction-journal read snapshots, and generated autoincrement gap preservation after direct/prepared transaction rollback, direct savepoint rollback, MEMORY/HEAP volatile statement, transaction, and savepoint rollback, failed/ignored generated inserts including durable first-key generated reservation gaps and source-driven failed/ignored `INSERT ... SELECT` reservation growth, failed `INSERT ... VALUES` and `INSERT ... SELECT` ODKU generated reservation preservation, explicit high-value duplicate-insert non-consumption, mixed generated `INSERT IGNORE` reserved tail gaps, FK-protected multi-row update non-consumption, successful explicit high-value insert/update rollback preservation, prior-success failed-update explicit autoincrement preservation across representative FK, duplicate-key, and CHECK failures, successful explicit high-value `INSERT ... VALUES` and `INSERT ... SELECT` `ON DUPLICATE KEY UPDATE` rollback preservation, and explicit rejection coverage for global autocommit, global transaction-variable, global `completion_type`, direct marker, expression-valued, global/expression parameterized transaction-control `SET`, bound `DEFAULT` / `RELEASE`, duplicate `SET TRANSACTION` characteristic forms, consistent snapshots, release completion, release completion-type defaults, XA, and durable direct or prepared DDL inside active transactions are covered; broader SQL rollback and broader failed-DML autoincrement matrices, WAL/checkpoints, and full storage isolation remain. |
| 15 | Locking and concurrency | 🟡&nbsp;In&nbsp;progress | Advisory primary-file locks reject unsafe cross-process readers, writers, and recovery races, configured busy timeouts wait for cooperating lock conflicts before returning busy, and representative SQL locking surfaces are rejected until real SQL lock semantics exist; SQL lock integration and full concurrent writers remain. |
| 16 | Compatibility harness | 🟡&nbsp;In&nbsp;progress | Group existing public API, storage, recovery, locking, embedded lifecycle, SQL API comparison including direct/prepared/typed prepared expression result sets, sidecar, routed SQL, transaction-control, transaction-hooks, statement-rollback, partition, foreign-key, foreign-key handler metadata, foreign-key create-info metadata, foreign-key DDL publication, CHECK-constraint, generated-column, unsupported-index, and server-surface including binlog/replication, replication/binlog filter assignments, binlog/replication system-variable assignments and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL file-I/O, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, routine debug-code inspection, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, view runtime, stored-program runtime, trigger runtime, dynamic UDF runtime, and non-table object tests by compatibility surface; an opt-in embedded MTR smoke runner with baseline and storage-routed modes, pass-gated run/probe workflows, skip-aware probe summaries, reject cleanup, timeout control, coverage inventory, batched support-target build preparation, and suite-batched strict runs covers the trimmed bootstrap schema plus no-default-database connection state, scalar CAST/CONVERT, CASE-family and ANSI SQL-mode expression behavior, selected numeric, hex-hybrid literal/rounding, integer metadata/rounding, character/varchar, binary-string, binary/varbinary, national-character, interval, and type behavior, selected date, temporal-rounding, temporal-function, alternate-timezone conversion, leap-second timezone conversion, system-timezone behavior, temporal literal behavior, temporal scale, high-resolution temporal function, microsecond parsing, and time/datetime comparison behavior, selected parser/comment, alias, wildcard-alias, keyword, CTE-cycle, precedence, expression, type-coercion, rounding, safe-update, negation-elimination, and comparison behavior, selected boolean aggregate/HAVING expression behavior, selected `IN` / `NOT IN` predicate behavior, selected subquery, subquery `ANALYZE FORMAT=JSON`, update-ignore, `REPLACE`, `RETURNING`, temporary create-or-replace, primary-key lookup, selected DDL/name and comment metadata behavior, selected table-close lifecycle under `RENAME TABLE` / `FLUSH TABLES`, selected MTR-profile view, trigger, and stored-procedure DDL/runtime behavior, selected DDL constraint/index metadata and UCA CTAS behavior, selected Aria ALTER/index-upgrade behavior, selected lock-table DDL behavior, selected deprecated server syntax rejection, selected embedded-profile native-engine absence, selected embedded-profile disabled diagnostic behavior, selected embedded-profile disabled metadata behavior, selected embedded-profile host-file SQL I/O rejection, selected embedded-profile optional SQL function absence, selected embedded-profile dynamic-column disabled fallback behavior, selected embedded-profile disabled SQL-surface behavior, selected storage-routed explicit MyLite and engine-alias DDL/DML behavior, requested-engine `SHOW CREATE TABLE` metadata, explicit MyLite rollback/commit, MyLite-owned schema sidecar absence, and representative raw storage-routed FULLTEXT, SPATIAL, vector, and long-unique rejection, selected `ORDER BY` optimizer and aggregate-ordering behavior, optimizer-cost metadata, selected EXPLAIN plan output, selected Aria range, semijoin, and rowid-filter optimizer behavior, selected UNION, EXCEPT / EXCEPT ALL, INTERSECT, and mixed set-operation behavior, prepared-statement behavior, selected compound-statement parser diagnostics, selected missing-routine diagnostics, selected SIGNAL/RESIGNAL diagnostics and row-constructor behavior, selected scalar `LAST_VALUE()`, window `FIRST_VALUE()` / `LAST_VALUE()`, percentile/median, and window-function behavior, selected `IF()` / `NULLIF()` conditional expression behavior, selected SET-family scalar-function behavior, selected scalar operator behavior, selected bit/extraction/replacement/regexp scalar-function behavior, selected default-expression and weight-string behavior, selected string/format function and charset-conversion expression behavior, selected crypto/KDF, disabled DES, and JSON equality/normalization behavior, selected aggregate DISTINCT and indexed count-distinct behavior, selected BIT-key, ODKU, and strict HEAP autoincrement behavior, selected date-format behavior, selected ASCII, legacy, UTF-32, filesystem, Latin2, UTF-8 binary/general, UTF-8 UCA 1400, and utf8mb3 general-1400 charset edge behavior, selected charset weight-string, LIKE condition-propagation, multibyte recoding, and charset CREATE/ALTER inheritance and conversion behavior, selected charset diagnostics, collation-default behavior, UTF-32 `character_set_collations`, column/context collation behavior, selected locale formatting, selected system-variable capability/local-infile/security behavior, and selected support-tool/mysqltest behavior outside the default groups; a known unsupported MTR inventory records exact-probed and selector-expanded non-coverage candidates by reason; executable MTR-scale comparison with broader unsupported-surface normalization and broader application suites remain. |
| 17 | Application schemas | 🟡&nbsp;In&nbsp;progress | Broader WordPress-shaped core-table smoke coverage is implemented for options, posts, postmeta, users, usermeta, terms, taxonomy relationships, comments, commentmeta, and links with representative `utf8mb4_unicode_ci` defaults; WordPress 6.9.4 single-site installer DDL and installer seed fixtures import through omitted-engine routing, including the full deterministic single-site `populate_options()` option-name inventory, selected default values and autoload flags, and the full default role payload; WordPress 6.9.4 multisite global and blog-id-2 schema plus representative network seed fixtures import through omitted-engine routing with indexed network and per-blog rows; BuddyPress 14.4.0 full-component plugin schema and representative rows import through omitted-engine routing with indexed activity, notification, friends, groups, messaging, xProfile, blog-tracking, invitation, and opt-out rows; Laravel v13.6.0 default ORM schema and representative rows import through omitted-engine routing with indexed user, session, cache, job, batch, and failed-job rows; Django 6.0.5 default auth, admin, contenttypes, sessions, and migration-recorder schema plus representative rows import through omitted-engine routing with indexed migration, permission, user, session, and admin-log rows; Rails v8.1.3 Active Record metadata, Active Storage, and Action Text schema plus representative rows import through omitted-engine routing with indexed migration metadata, blob, attachment, variant, and rich-text rows; a representative collation restart/index matrix covers selected utf8mb4, utf8mb3, latin1, latin2, and cp1250 collations including MariaDB 11.8 UCA 1400 defaults; full WordPress runtime install, dynamic PHP/theme/localization installer output, exhaustive collation suites, broader ORM suites and migration runners, Laravel/Django/Rails runtime integration, full multisite runtime, additional per-blog suites, BuddyPress runtime activation, WordPress `dbDelta()` plugin execution, and additional plugin schemas remain. |
| 18 | Server-surface policy | 🟡&nbsp;In&nbsp;progress | Runtime defaults disable networking, grants, binlog, events, and host plugin discovery; representative server SQL plus replication/binlog filter assignment, binlog/replication system-variable assignment and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL host-file import/export, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection including known external no-equals engine names, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, routine debug-code inspection, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, dynamic UDF DDL, and non-table object rejection smoke is implemented; the default profile compiles out dynamic plugin loading, Performance Schema, socket auth, feedback, thread-pool info, userstat, host-file SQL I/O, unsupported server utility functions, the Oracle SQL mode parser, XML SQL functions, GIS SQL functions, vector SQL and MHNSW runtime, SFORMAT, JSON schema validation, JSON table-function execution, dynamic-column packed BLOB runtime, and the full SQL HANDLER, SQL sequence runtime, virtual SEQUENCE storage engine, SQL HELP, PROCEDURE ANALYSE, generic SELECT procedure, view runtime and sidecar metadata, stored-program runtime, event parse-data validation, trigger runtime and sidecar metadata, dynamic UDF runtime, binlog transaction/event-root, log-event server, replication GTID-state runtime, replication filter runtime, binlog/replication system-variable registration for disabled topology features, MyISAM maintenance, native CSV, InnoDB, MyISAM, MRG_MyISAM, and partition engine registration, native-MyISAM-only system variables, foreign-server metadata, external backup SQL, query cache runtime, zlib-backed compression, statement profiling class bodies, optimizer trace runtime, static SHOW information producers, status metadata producers, process-list metadata producers, and routine metadata producers. |
| 19 | Size profile hardening | 🟡&nbsp;In&nbsp;progress | Archive and linked-runtime size reporting is implemented, server-surface trims are measured, and the embedded profile now omits unsupported `LOAD DATA` / `LOAD XML` execution, SQL host-file I/O bodies, low-value server utility SQL functions, the Oracle SQL mode parser, XML SQL functions, GIS SQL functions, vector SQL and MHNSW runtime, SFORMAT, JSON schema validation, JSON table-function execution, dynamic-column packed BLOB runtime, dynamic plugin loading, and the full SQL HANDLER, SQL sequence runtime, virtual SEQUENCE storage engine, SQL HELP, PROCEDURE ANALYSE, generic SELECT procedure, view runtime and sidecar metadata, stored-program runtime and sidecar metadata, event parse-data validation, trigger runtime and sidecar metadata, dynamic UDF runtime, binlog transaction/event-root core, log-event server runtime, replication GTID-state runtime, replication filter runtime, binlog/replication system-variable registration for disabled topology features, MyISAM maintenance, native CSV, InnoDB, MyISAM, MRG_MyISAM, and partition engine registration, native-MyISAM-only system variables, user-statistics plugin, foreign-server metadata, external backup SQL runtime, query cache runtime, zlib-backed compression, statement profiling class bodies, optimizer trace runtime, static SHOW information producers, status metadata producers, process-list metadata producers, and routine metadata producers, including `gtid_index.cc`, `log_event.cc`, `log_event_server.cc`, `rpl_gtid.cc`, `rpl_filter.cc`, `rpl_injector.cc`, `rpl_record.cc`, `mi_check.c`, `mi_keycache.c`, `mi_preload.c`, `ha_innodb.cc`, `handler0alter.cc`, `srv0start.cc`, `trx0trx.cc`, `row0mysql.cc`, `dict0dict.cc`, `buf0buf.cc`, `fil0fil.cc`, `fsp0fsp.cc`, `log0log.cc`, `ha_myisam.cc`, `mi_open.c`, `ft_myisam.c`, `ha_myisammrg.cc`, `myrg_open.c`, `ha_tina.cc`, `transparent_file.cc`, `ha_partition.cc`, `item_vectorfunc.cc`, `vector_mhnsw.cc`, `json_schema.cc`, `json_table.cc`, `ma_dyncol.c`, `sql_handler.cc`, `sql_sequence.cc`, `ha_sequence.cc`, `sequence.cc`, `sql_view.cc`, `event_parse_data.cc`, `sql_trigger.cc`, `userstat.cc`, `sql_servers.cc`, `backup.cc`, `sql_cache.cc`, `emb_qcache.cc`, `opt_trace.cc`, `authors.h`, and `contributors.h` in the disabled profile, with retained `sql_embedded` C++ sources compiled without exceptions, linked first-party embedded smoke binaries no longer linking libz, and generated embedded configs clearing the host dynamic-loader probes; deeper daemon-only and low-value optional component trims remain. |

Recent row and index storage work also covers grouped later-in-key
autoincrement rollback for direct transactions, nested savepoints, prepared
inserts, rolled-back explicit high rows, routed `ENGINE=InnoDB`, and
close/reopen. This narrows the grouped autoincrement transaction gap while
leaving storage-level B-tree prefix lookup as planned work.
Durable grouped autoincrement allocation now also reads the matching serialized
key-prefix entryset from storage, using published leaf roots when present and
falling back to append-tail scanning for live row-state overlays.
Runtime-volatile MEMORY/HEAP grouped autoincrement now uses the equivalent
volatile prefix-entryset read before handler-side maximum selection.
Durable prefix entryset coverage now includes multi-page published leaf runs
for full-key, shorter-prefix, missing-prefix, and append-tail overlay probes.
Handler cursor construction now uses those prefix entryset readers for
byte-safe non-null integer key prefixes that end on a complete key-part
boundary, avoiding whole-index entryset materialization for composite integer
prefix lookups. Durable byte-safe forward range starts now read a lower-bound
entryset suffix from published leaf or branch roots before handler positioning,
while strings, nullable keys, partial key-part prefixes, volatile rows, and
reverse range-neighbor reads stay on the conservative full cursor path.

The opt-in MTR smoke runner also covers selected ODBC compatibility syntax,
optimizer-trace default metadata, SHOW row-order, system `mysql` table
reference, long-tmpdir view, slow-log variable and general-log path state, and
deprecated rename-database diagnostics plus selected system-variable metadata,
path, charset/collation, cache/limit, function-style mutation, session-control,
default/version behavior, static global variable metadata, read-only, SQL-mode,
timestamp, transaction-compatibility, SHOW metadata, selected `funcs_1`
information-schema metadata, and optimizer-regression behavior, while capping
current accepted upstream MTR coverage at 440 of 5,901 imported
upstream test files through the harness inventory, with 4,617 upstream MTR
files recorded separately as known unsupported/profile-mismatched non-coverage
through exact probes or suite selectors, including source-backed `funcs_1`
stored-program, trigger, view, processlist, privilege, event, and routines
metadata families, selected privilege-filtered metadata probes, plus
additional binlog and replication system-variable families, selected
server-surface system variables,
native-engine and daemon-log system-variable probes, exact main debug-only
and profiling probes, exact main host-file startup/import and symlink sidecar
probes, exact main network/TLS/thread, protocol, binlog, replication,
statistics-table replication/binlog, and query-cache probes, exact main
optimizer-trace probes, exact main
status/show-explain/account/routine metadata probes, exact main grant/account,
slow-query-log, foreign-server restart, KILL/processlist debug, and external
utility, key-cache/preload, native upgrade fixture, packet/idle-timeout
protocol behavior, daemon shutdown/SIGHUP, bootstrap, large-page, Windows
service, and thread-pool probes, global read-only account-policy probes,
information-schema privilege/view/routine probes, system-table and
mysql_upgrade fixtures, Windows-only filesystem and console-codepage probes,
exact partition DDL/metadata probes, information-schema
log-table/processlist/userstat/view/stored-function probes, dynamic plugin
exact probes, event prepared-statement missed-command probes, trigger row-skip
runtime, view-lock/mysqldump behavior, host-file export/import round trips,
datadir permission and temporary `.frm` probes, tmp-table and
native-FRM/engine status accounting, delayed long-unique insert behavior,
query-cache InnoDB, and unsupported funcs_1 engine metadata probes, and selected
engines-suite
stored-program, trigger, and native InnoDB probes, plus main trigger and view
families, selected main stored-procedure probes, and main native
InnoDB/MyISAM/FULLTEXT, log-table, mysqltest self-test, external client
utility, daemon utility, funcs_1 native-engine metadata, and main
account/privilege, client/server protocol, server-feature DDL,
bootstrap/service utility, native sidecar metadata, legacy
charset/temporal/varchar upgrade fixtures, account authentication,
session kill/processlist, SQL HELP, SELECT PROCEDURE, sequence runtime,
statement profiling, plugin-install, routine-metadata, stored-function
rollback/account runtime leftovers, administrative FLUSH/FTWRL,
table-maintenance/key-cache, legacy `.frm`, online/in-place ALTER,
zlib compression, DES/SFORMAT, `INSERT DELAYED`, static SHOW, plugin
inventory/origin, main `--big-test`, big-processlist, upstream-disabled MEMORY
placeholder, packaging-file, and daemon-status probes, plus generated/virtual-column
JSON_TABLE, replication/binlog, query-cache, file-I/O, partition, trigger,
view, external dump, optional-engine, SQL HANDLER, and exact generated-column
InnoDB debug/purge/restart/statistics leftovers, virtual-column upgrade
sidecars, native MyISAM key/repair coverage, debug-only virtual-column probes,
and exact optimizer-unfixed debug probes. The
storage-routed MTR smoke runner also
covers selected explicit MyLite and engine-alias DDL/DML routing,
requested-engine `SHOW CREATE TABLE` metadata, sidecar absence, representative
plain/LIKE/CTAS `CREATE OR REPLACE TABLE` replacement and pre-drop
missing-source LIKE replacement preservation, catalog-backed
multi-schema lifecycle behavior, routed same-name tables,
`CREATE TABLE IF NOT EXISTS`, `RENAME TABLE IF EXISTS`, and
`DROP TABLE IF EXISTS`, explicit
MyLite rollback/commit and DML statement effects, and routed `InnoDB`
rollback, commit, savepoint, representative foreign-key behavior, and
representative `SET NULL` / `CASCADE` foreign-key actions against a static
MyLite storage-engine build with a primary `.mylite` file, including explicit
MyLite foreign-key publication, enforcement, action behavior, and
`foreign_key_checks=0` row-DML and parent-truncate bypass. It
also covers representative routed `InnoDB` and explicit MyLite CHECK and
generated-column metadata, enforcement, generated values, generated-index
reads, and generated unique-key diagnostics through the same raw embedded
storage path, plus representative routed `InnoDB` and explicit MyLite DDL
lifecycle behavior for LIKE, CTAS, copy `ALTER`, indexed reads after rebuild,
and `RENAME TABLE`, plus representative
routed `InnoDB` DML statement effects for ODKU, `REPLACE`, keyed `UPDATE`,
keyed `DELETE`, affected rows, insert ids, and final indexed visibility, plus
representative large `TEXT` / `BLOB` payload reads, bounded prefix-index forced
reads, large-value update filtering, and unique `TEXT` prefix rejection, plus
representative routed `InnoDB` and explicit MyLite autoincrement generated ids,
explicit high-value advancement, rollback gaps, offset/increment values, and
truncate reset, plus representative unsupported engine request rejection and
failed table metadata absence, plus representative unsupported FULLTEXT/SPATIAL
index rejection, plus representative raw MEMORY/HEAP volatile transaction,
savepoint, failed-statement rollback, and autoincrement gap behavior, plus
representative raw partition-definition rejection and failed table metadata
absence, plus representative raw online and in-place ALTER rejection with
copy-ALTER preservation and blocked-column metadata absence, plus
representative raw temporary LIKE, CTAS, same-name shadowing, and post-drop
metadata cleanup, plus representative raw ignored secondary-index metadata,
forced-hint rejection, copy-ALTER toggling, and `SHOW CREATE TABLE` output.
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
It also includes indexed `COUNT(DISTINCT ...)` and leap-second timezone
conversion coverage.
It also includes selected baseline FULLTEXT declaration, Aria FULLTEXT
update/cache, and Aria FULLTEXT search coverage while MyLite routed storage
continues to reject unsupported FULLTEXT index definitions explicitly.
It also includes selected locale formatting plus `my_print_defaults`,
`mysqltest`, and `perror` support-tool coverage.
It also includes selected system-variable capability, local-infile, security,
general-log path, initialization, FULLTEXT syntax, and static global metadata
coverage, plus the remaining retained `sys_vars` read-only, SQL-mode,
timestamp, transaction-compatibility, and SHOW metadata probes. It also
includes selected retained `funcs_1` information-schema metadata and an
optimizer-unfixed regression probe.

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
inserted after the root reaches capacity. Prepared row-only updates now also
preserve exact-key visibility after maintained roots overflow by using the
existing row-state overlay when a physical single-page root retarget no longer
applies. Contiguous raw fixed-width leaf runs now serve exact byte-key base
snapshots with page-range lookup and append-tail visibility overlay, including
full durable index reads that can start from the published run instead of
scanning the whole append history. Storage now also has a validated branch-page
format and publishes single-level branch roots for rebuilt fixed-width leaf
runs that fit in one branch page, using high `(key, row_id)` fences for
exact-key and prefix lower-bound child selection and following the stored child
page ids rather than assuming contiguous leaves.
The local performance harness now also measures direct and prepared
published-root secondary range `LIMIT 1` reads, including matching append-tail
overlay phases, giving the next key-navigation slices a focused SQL-level
baseline. Inline insert append pages now reserve eligible active append-buffer
ranges before encoding, avoiding the extra temporary page-run copy in prepared
insert transactions while preserving the direct-write fallback path. Repeated
active inserts now also reuse table-id and index-root absence caches, avoiding
catalog reloads for tables that have already proven they have no maintained
index roots in the current checkpoint. Active exact-key duplicate probes can
now reuse a complete durable exact-index cache as the committed base while
recording same-statement inserts in an active append overlay, avoiding complete
cache copies in prepared autocommit insert loops without hiding same-statement
duplicates. Prepared insert component timing now also reports final commit cost,
so append-buffer and page-layout changes can distinguish per-row execution work
from deferred top-level publication work. It now also reports branch-maintenance
counter tables in the storage-smoke test-hook profile, so follow-up
prepared-insert work can distinguish refold reads, cache hits, planning reads,
writer decodes, tail-overlay scans, and packed-tail scan blocker categories in
the same run. Prepared update component timing now also reports storage wrapper
counters for active-statement indexed-row reads and index-preserving update
writes plus row-update path counters for maintained-root plans, active buffered
rewrites, inline writes, and fallback append writes, giving the next update
hot-path slices comparable evidence before changing SQL, handler, or storage
mutation behavior. Maintained-root inserts
in active checkpoints now buffer repeated single-page root rewrites and flush
them at the checkpoint boundary, while immediate update/delete root writes
discard stale buffered copies for the touched page. Full leaf-page preparation
now lazily allocates raw-order arrays only after the first out-of-order entry,
preserving direct entryset-order encoding for already-sorted refold inputs and
the existing sorted output for out-of-order refold and rebuild inputs.
Simple branch-leaf inserts now append to existing branch-refold entryset caches
instead of discarding them, while structural branch rewrites still invalidate or
replace those caches. Same-root single-level leaf splits now preserve those
logical refold entryset caches as sorted inserts instead of discarding them with
broader structural rewrites. Branch leaf-entry readers now probe the active
leaf-page cache before falling back to durable or pager reads, avoiding redundant checksum
decode work for child leaves already written by the active statement. Deeper
branch insert planning now uses the same active leaf and branch page caches for
selected descendant reads, so same-statement branch descent can reuse
just-written split and fence pages before falling back to durable decode.
Active index page caches now skip linear scans for ascending new page ids and
recycle full cache slots in place, reducing snapshot-write cache maintenance
overhead. The active index leaf-page cache now retains a larger bounded working
set so prepared insert branch planning can reuse more sibling leaf probes before
falling back to durable reads. Level-`2` branch insert planning now also uses
validated lower-branch entry counts to skip selected leaf reads when the lower
branch is already packed and the planner can enter the existing full-leaf split
path directly. Active leaf and branch page caches now keep transient page-id
buckets for retained page lookups, avoiding linear scans when branch planning
reuses non-last cached pages.
Decoded branch page views now also carry their validated maximum child page id,
so branch-tail overlay checks can reuse that boundary instead of rescanning
child cells.
Leaf-range redistribution now skips raw-order array construction when the
collected range entryset is already sorted, while preserving the existing sort
fallback for unsorted inputs.
The prepared-insert benchmark now reports checksum-call and raw-entry ordering
counters, giving the next checksum-focused insert slice direct run evidence.
Leaf-range redistribution now keeps the collected entryset sorted while adding
the inserted row, so the range encoder can skip both raw-order builds and
raw-order probes for that writer path.
Prepared-insert checksum counters now also break aggregate checksum calls down
by page family, giving checksum-focused follow-up slices direct evidence about
which storage page types still dominate local runs.
Catalog image initialization now skips the durable empty-page checksum when the
buffer is only a transient record container, while durable catalog pages still
refresh checksums before file publication.
Prepared-insert checksum page-family counters now include dirty-buffer refresh
counts, separating fresh encode checksum calls from deferred dirty-page
refreshes in the same benchmark output.
Prepared-insert dirty checksum refresh counters now also report the refresh
source, separating dirty-page flush, append-buffer flush, direct maintained
writes, copy-for-read, and test-hook refreshes before the next lifecycle
optimization changes timing.
Dirty-page copy-for-read now refreshes the buffered page itself and clears its
checksum-dirty flag, so pages read before flush are not rechecksummed at flush
unless a later write dirties them again.
Prepared-insert checksum counters now snapshot insert-loop, commit, and
verification deltas, so later hot-path work can separate timed step work from
one-shot commit flushes and final validation reads.
Prepared-insert checksum phase output now also breaks those phase deltas down
by page family, identifying row, leaf, and branch checksum work in the timed
insert loop separately from commit and verification.
Dirty-refresh benchmark output now also joins refresh source with page family
and phase, identifying which page families are refreshed by dirty-page flush,
append-buffer flush, copy-for-read, and related refresh sources.
Prepared-insert checksum call-site counters now attribute full-page and
zero-tail checksum calls by caller function and page family. The current
storage-smoke profile reports `8` full-page calls, `227,063` zero-tail calls,
`0` index-leaf page clears, and `0` encoded index leaf max-cell reads. The
same run sampled `73.109 us/op` and reports `2` raw entry order builds plus
`668` raw entry order probes after split writers began carrying ordered
entrysets into split-page encoding. `5` `index-root` full-page checksum calls
come from `decode_maintained_index_root_page`, and the final verification scan
accounts for `107,078` zero-tail `row` calls in `decode_row_page_metadata`.
Index-leaf
encode-site counters now split the `25,572`
`encode_zeroed_index_leaf_page` zero-tail calls into `24,796` pages from
`prepare_zeroed_index_leaf_range_pages`, `772` from
`prepare_index_leaf_split_pages`, and `4` from
`prepare_index_branch_snapshot_pages_with_order`. Single-level branch
leaf-range redistribution now allocates its fresh replacement leaf run as
zeroed pages and uses a zeroed range encoder, avoiding the generic encoder's
full-page clears without moving leaf checksum calculation or dirty-buffer
publication. The zeroed range encoder now carries each replacement leaf's max
fence into branch refresh, so branch refresh no longer rereads the `24,796`
range-encoded replacement leaves to recover their max cells. The split-leaf
encoder now carries its two replacement max fences into branch child copying,
so `copy_index_branch_children_with_split` no longer rereads `772` encoded
split leaf pages. The generic index-leaf encoder now zeroes only the unused
tail for reused or stack buffers, removing the remaining full-page leaf clears
from split-page encoding while keeping checksum bytes unchanged. Branch
snapshot preparation now carries max fences from the zeroed leaf-run encoder
into `encode_index_branch_page_from_leaf_run`, leaving the encoded leaf
max-cell read call-site table at `none | 0`.
Single-level branch leaf-range
redistribution also
defers branch checksum refreshes through the dirty buffer, removing
`refresh_index_branch_children_after_leaf_range_redistribution` from the
`index-branch` zero-tail call-site table. Single-level branch leaf splits now
also assemble the rewritten existing branch root without an immediate checksum
and stage it through the checksum-dirty maintained writer, removing
`encode_index_branch_page` from the prepared-insert branch zero-tail call-site
table and leaving only `4` `index-branch` zero-tail calls in the current
profile.
Maintained-root decode call-site counters now split those `677` root decodes by
the caller that requested validation and by checksum state. The current profile
reports `2` full-checksum decodes under `validate_recovery_journal_saved_page`,
`674` under `plan_maintained_index_root_inserts` (`2` full checksum, `672`
checksum-dirty), and `1` full-checksum decode under `read_index_leaf_run_root`.
The duplicate packed-admission validation helper, duplicate single-page root
insertion decode, and writer-side overflow-tail mark/promotion decodes have
been removed, and recovery-journal extension now reuses the statement's already
validated journal snapshot instead of decoding and revalidating old saved
pages. Planned maintained-root insert writes now carry checksum-dirty local
root pages into the dirty buffer, and subsequent planning decodes validate
those local root bytes without forcing an immediate checksum refresh.
Overflow-tail marking and branch promotion reuse those planned dirty root bytes
too, leaving the current profile with `0` `dirty-page-copy` root refreshes;
durable root reads and newly added journal protected pages remain
checksum-validating gates.
Prepared-insert index-branch decode-site counters now split the remaining
aggregate branch decoder checksums by caller. Branch leaf splits now replace
their post-encode full branch decode with targeted branch encoder input
validation, so the current profile reports `none | 0` branch decode sites and
`0` `index-branch` full-page calls while preserving newly added
recovery-journal protected-page validation.
Dirty-page publication checksum-source counters now split the broad
`dirty-page-flush` refresh bucket by publication path. The current
prepared-insert smoke profile reports `21,031` buffer-limit `index-leaf`
refreshes, `1` statement-commit `index-leaf` refresh, `2` statement-commit
`index-branch` refreshes, and `66,144` merge-direct-write `index-leaf`
refreshes, exactly reconciling the `87,178`
`dirty-page-flush` refreshes while
preserving the existing aggregate refresh-source table.
The dirty-page buffer now uses the full journal protected-page window instead
of flushing at half of it, reducing repeated maintained-page publication inside
prepared insert loops without changing the durable journal format.
Prepared-insert dirty-page buffer output now also reports flush calls and pages
by trigger, separating buffer-limit pressure from root statement commit flushes.
Dirty-page buffer flush output also breaks published pages down by page family,
giving the next eviction-policy slice visibility into leaf and branch pressure.
Dirty-page buffer flush write-site counters now identify the maintained writer
that originally buffered each flushed page, split by flush source, page family,
and checksum-dirty state, so pressure-policy work can compare forcing pages
with selected victims.
Buffer-limit pressure output now also records the incoming page family and
checksum-dirty state admitted after each pressure flush, so profiles can
compare eviction victims with the pages that force pressure.
Dirty-page buffer replacement output now records page families and
checksum-dirty state for rewrites of pages already resident in the dirty
buffer, giving checksum-timing work replacement-churn evidence.
Dirty-page replacement write-site counters now further attribute those
in-buffer rewrites by maintained writer and page family in test-hook benchmark
output, identifying which branch-insert writers are repeatedly touching hot
leaf and branch pages.
Dirty-page buffers now add transient page-id buckets once the active protected
window is large enough, keeping repeated maintained-index replacement, merge,
read, and discard lookups off the full linear scan while preserving the
existing nested-statement rollback and flush behavior.
Dirty-page copy output now attributes buffered-page copy hits to direct reads,
pager reads, or dirty-page undo capture, separating the remaining copy-for-read
refreshes by read context. The current prepared-insert smoke profile has no
hot-path `dirty-page-copy` refreshes.
Pager-read site copy counters now split those pager-read dirty-buffer hits by
caller function and page family in test-hook benchmark output. The current
prepared-insert profile has no dirty pager-read copy site rows.
Branch leaf-range redistribution now reads its selected branch and child leaves
through cache-aware branch insert writer helpers, giving active maintained pages
the same copy-refresh avoidance used by other branch writer paths. The smoke
profile removes redistribution pager-read site rows and leaves the remaining
redistribution-related leaf copies visible under dirty-page undo capture.
Branch leaf splits now use the same cache-aware branch and leaf writer readers
for their initial page reads, separating split read-path refreshes from later
undo-capture refreshes in the prepared-insert counters. The smoke profile now
has no dirty pager-read copy rows; remaining dirty copy hits are undo-capture
preimages for maintained leaf and branch writes.
Dirty-page undo write-site counters now attribute those undo-capture
dirty-buffer copy hits by maintained writer caller and page family in test-hook
benchmark output, including the prevalidated leaf writer. The current smoke
profile keeps the aggregate `664` dirty leaf undo-capture copies visible as
`654` copies from branch leaf-range redistribution and `10` from branch leaf
splitting.
Dirty-page undo capture now copies dirty-buffer rollback preimages without
refreshing the live dirty-buffer checksum, carrying a transient dirty flag on
the undo entry and repairing the checksum only if rollback restores that
preimage. The prepared-insert smoke profile now reports `0`
`dirty-page-copy` checksum refreshes while retaining the same undo-capture
copy attribution.
Buffer-limit dirty-page pressure now publishes one buffered maintained-index
page at a time, keeping the rest of the fixed window hot across insert-loop
pressure events.
Dirty-page buffer flush leaf-shape counters now split flushed index leaves into
clean, dirty partial, and dirty full buckets by source, giving pressure-policy
follow-up work direct evidence about the remaining leaf victims.
Dirty-page buffer flush leaf fill-band counters now add occupancy buckets for
flushed index leaves, so follow-up pressure designs can distinguish sparse
partial victims from near-full victims without changing production behavior.
Dirty-page buffer flush leaf free-slot counters now split those victims by
remaining capacity. The current prepared-insert smoke profile reports `14`
full buffer-limit leaf victims, `118` victims with `1-15` free slots, and
`20,899` victims with `16+` free slots, showing that most high-fill pressure
victims still have meaningful remaining capacity.
Dirty-page buffer flush leaf replacement-state counters now split flushed
index leaves into never-replaced, replaced-once, and replaced-multiple buckets.
The current prepared-insert smoke profile reports `15,389` buffer-limit leaf
victims as `never-replaced`, `3,647` as `replaced-once`, and `1,995` as
`replaced-multiple`, pointing checksum follow-up work at first-admitted dirty
leaves rather than repeatedly rewritten leaves.
The replacement-state/fill-band matrix now joins those states to occupancy:
the same profile reports `11,961` never-replaced `75-99%` leaves, `7`
never-replaced full leaves, and no buffer-limit flushes below the `50-74%`
band, confirming that the remaining flush work is high-occupancy
first-admitted leaves rather than sparse leaf churn.
The replacement-state/free-slot matrix now adds exact remaining-capacity
evidence: among `0-15` free-slot buffer-limit victims, `94` were
never replaced, `8` were replaced once, and `30` were replaced multiple
times before publication.
Pressure incoming leaf fill-band counters now classify the index leaves
admitted after each buffer-limit flush with the same occupancy buckets. The
current prepared-insert smoke profile reports `16,793` incoming leaves in the
`75-99%` band, `4,219` in `50-74%`, `19` in `1-24%`, and no full incoming
leaves after near-full and `16-31` future-current leaves direct-write instead
of entering the parent dirty buffer.
Incoming leaf free-slot counters now split those pressure admissions by exact
remaining capacity. The current profile reports no incoming leaves with `0-15`
free slots and `21,031` leaves with `16+` free slots, showing the bounded
near-full and `16-31` direct-write policy removed the next pressure-admission
class while preserving broader coalescing leaves.
Incoming leaf free-slot detail counters now split that remaining `16+` class
into narrower ranges. The current prepared-insert smoke profile reports no
pressure incoming leaves with `16-31` free slots, `11,623` with `32-63`,
`7,430` with `64-127`, and `1,978` with `128+`.
Dirty-page pressure admission-source counters now separate direct
dirty-buffer stores from child-statement dirty-buffer merges. The current
prepared-insert smoke profile attributes all `21,031` buffer-limit pressure
admissions to `dirty-buffer-merge`, all as dirty `index-leaf` admissions, with
no `direct-store` or `index-branch` rows.
That points follow-up pressure work at nested dirty-buffer merge rather than
the direct pager admission path.
Dirty-page pressure admission entry replacement-state counters now show
whether those merge-sourced incoming pages were rewritten in the child dirty
buffer before merge. The current prepared-insert smoke profile attributes all
`21,031` pressure admissions to `never-replaced` dirty `index-leaf` child
entries, with no `not-buffered-entry`, `replaced-once`, `replaced-multiple`, or
`index-branch` rows, pointing a future merge admission policy at
first-admitted child pages.
Dirty-page buffer replacement leaf fill-band counters now add the same
occupancy buckets for in-buffer leaf rewrites, exposing whether replacement
churn matches the high-fill pressure victims.
Dirty-page buffer replacement leaf change-class counters now separate
append-only, interior single-entry insert, same-shape, shrink/refold,
identical, invalid, and other index-leaf rewrites before overwrite, giving leaf
fast-path follow-up work direct shape evidence. The current prepared-insert
smoke profile reports `4,793` append-only leaf replacements and `29,755`
interior single-entry insert leaf replacements, with no identical, same-shape,
shrink, invalid, or other valid leaf replacements. Byte-proven single-cell
leaf growth replacements now update the resident dirty-buffer slot in place
instead of copying the full page; class-specific fast-path output reports
matching `4,793` append and `29,755` insert fast replacements, reconciling
exactly with the `34,548` leaf growth fast replacement total. The test-hook
replacement path now classifies each incoming leaf replacement once and reuses
that class for the replacement and merge-fallback counter families without
changing production dirty-buffer behavior.
Dirty-page buffer replacement branch-level counters now split in-buffer branch
rewrites by maintained-tree level and checksum-dirty state, exposing whether
branch churn is lower-branch or upper-fence propagation. The current
prepared-insert smoke profile reports all `129,543` branch replacements in
level-`1` pages, with `129,541` checksum-dirty replacements.
Dirty-page buffer branch replacement change-class counters now compare the
resident and replacement branch pages before overwrite, separating
entry-count-only rewrites from child-fence and structural changes while
ignoring deferred checksum bytes. The current prepared-insert smoke profile
reports `115,619` entry-count-only branch replacements, `13,922`
entry-count-plus-fence branch replacements, and no structural branch
replacements.
Entry-count-only branch replacements now update only the resident branch
checksum and entry-count fields, avoiding full-page copies while preserving the
same final buffered page image for the dominant branch replacement class.
Entry-count-plus-fence branch replacements now use the same resident in-place
strategy after proving branch shape, child page ids, and tail bytes are
unchanged.
Branch replacement fast paths now share a single proof helper for
entry-count-only and entry-count-plus-fence updates, so common branch layout and
fixed-header validation run once before the resident page is updated. The
prepared-insert counters stayed unchanged (`115,753` entry-count fast
replacements, `14,172` entry-count-plus-fence fast replacements, `8`
full-page checksum calls, `227,063` zero-tail checksum calls, and `677`
maintained-root decodes), and the sampled storage-smoke prepared insert step
moved from `71.218 us/op` to `70.064 us/op`.
Branch child-insert replacements now fast-replace the split-shaped structural
level-1 branch rewrites in place. The prepared-insert smoke profile reports
`386` branch child-insert fast replacements, matching the `386` structural
branch replacements from `split_branch_index_leaf_entry`, while replacement
family totals, `8` full-page checksum calls, `227,063` zero-tail checksum
calls, and `677` maintained-root decodes stayed equivalent.
Broad future-current guard context now gets the maximum resident parent leaf
page id and would-be pressure victim from one complete pressure-selection scan,
while the normal pressure selector keeps its first-clean-leaf early return.
The prepared-insert structural counters stayed unchanged (`21,031` dirty leaf
pressure admissions, `66,144` dirty leaf merge direct writes, `87,176`
index-leaf dirty refreshes, `8` full-page checksum calls, `227,063` zero-tail
checksum calls, and `677` maintained-root decodes), and the sampled
storage-smoke prepared insert step moved from `70.064 us/op` to `69.676 us/op`.
Broad fallback replay now carries the guard-selected pressure victim into the
immediate fallback writer for the same child entry, avoiding a second generic
pressure-selection scan when the parent buffer state has not changed. The
prepared-insert structural counters stayed unchanged (`21,031` dirty leaf
pressure admissions, `66,144` dirty leaf merge direct writes, `87,176`
index-leaf dirty refreshes, `8` full-page checksum calls, `227,063` zero-tail
checksum calls, and `677` maintained-root decodes), pressure-context output
reported `31,938` builds and `19,053` planned stores, and the sampled
storage-smoke prepared insert step was `71.366 us/op` under high host load.
Maintained-root dirty-buffer replacements now fast-replace proved one-cell root
inserts and overflow-tail header marks in place. The prepared-insert smoke
profile reports `666` maintained-root insert fast replacements and `2`
maintained-root overflow fast replacements, covering all `668` checksum-dirty
`index-root` dirty-buffer replacements. Planning and journal validation remain
intact, and the structural profile stayed equivalent at `8` full-page checksum
calls, `227,063` zero-tail checksum calls, and `677` maintained-root decodes.
Maintained-root planning now borrows resident dirty-buffer root pages by const
pointer for immediate validation decode instead of copying them into the
planner scratch page first. The prepared-insert smoke profile reduces
`index-root` direct-read dirty copies from `1,346` to `674` while preserving
the same `677` maintained-root decode sites, `8` full-page checksum calls, and
`227,063` zero-tail checksum calls.
Maintained-root writers now clone parent dirty-buffer roots directly into the
current statement dirty buffer before applying planned root inserts and
overflow-tail marks in place. Overflow promotion validation also borrows the
dirty root by const pointer. The current prepared-insert smoke profile removes
the remaining `674` `index-root` direct-read dirty copies, leaving the same
`668` checksum-dirty `index-root` replacement count, `677` maintained-root
decode sites, `8` full-page checksum calls, and `227,063` zero-tail checksum
calls.
Dirty-page pressure write-site counters now attribute buffer-limit incoming
pages by maintained writer and page family, including nested statement
dirty-buffer merges. The current prepared-insert smoke profile points all
`21,031` dirty `index-leaf` admissions at `insert_branch_index_leaf_entry`,
with no dirty `index-branch` pressure admissions.
Protected existing index-leaf pages merged from a child dirty-page buffer can
now publish directly without evicting a parent dirty-buffer victim when the
parent buffer is full and a parent-chain dirty-page undo preimage already
protects rollback. Branch pages deliberately stay on the buffered replay path
to preserve repeated branch replacement coalescing. Guard outcome counters keep
this existing-page path separated from future-current direct-write publication.
Merge direct-write guard counters now explain why child dirty-buffer merge
entries use direct write or fallback replay. The current smoke profile reports
`3,827` dirty `index-leaf` entries direct-written under the
`future-current-header-direct-write` guard, `31,312` entries direct-written
under `future-current-header-near-full-direct-write`, `18,120` entries
direct-written under `future-current-header-16-31-direct-write`, `4,218`
entries direct-written under
`future-current-header-replaced-broad-victim-direct-write`, `2,343` entries
direct-written under
`future-current-header-dense-broad-victim-direct-write`, `2,147` entries under
`future-current-header-equal-broad-victim-direct-write`, `2,058` entries under
`future-current-header-equal-dense-victim-direct-write`, `2,119` entries under
`future-current-header-wider-victim-direct-write`, `21,031` entries kept on
fallback as `future-current-header-partial-leaf`, `4,455` dirty
`index-leaf` entries blocked as `parent-not-full`, and no `missing-undo` leaf
rows, keeping new-page publication policy bounded to full, near-full, `16-31`,
already-replaced broad-victim, dense/equal victim, and strictly wider-victim
future-current leaves.
Guard leaf-shape counters now split merge direct-write guard outcomes by
index-leaf fill band and free-slot band. The current smoke profile reports
`3,827` full future-current direct-written leaves, `31,312` near-full
future-current direct-written leaves with `1-15` free slots, `18,120`
future-current direct-written leaves with `16-31` free slots, `2,343`
dense-victim direct-written leaves with `64-127` free slots, and `2,119`
wider-victim direct-written leaves. The remaining `21,031` partial
future-current fallback leaves all have `32+` free slots, keeping broader
still-growing leaves buffered for coalescing.
Guard free-slot detail counters split those fallback leaves into `11,623` with
`32-63` free slots, `7,430` with `64-127`, and `1,978` with `128+`. That keeps
the next direct-write candidate measurable without changing the current
fallback policy.
Future-page merge relation counters now split those `future-page` rows by
parent current-header coverage and parent/child append-buffer residency. The
current smoke profile reports all `122,388` dirty `index-leaf`
future-current relation rows as `within-current-header` and append relation
`none`.
Full, near-full, and `16-31` free-slot future-current merge leaves can now
direct-write under parent dirty-buffer pressure, while append-buffer-resident
pages, parent-resident pages, branch pages, pages past the parent current
header, and partial leaves with `32+` free slots still use fallback replay. A broad
future-current direct-write experiment regressed the prepared insert step to
`94.432 us/op`; the earlier bounded `0-31` free-slot policy run recorded
`68.775 us/op`, `53,136` dirty leaf direct writes, and `34,484` dirty leaf
pressure admissions.
A bounded `32-63` future-current direct-write experiment was not adopted: it
reduced dirty leaf pressure admissions to `15,263`, but increased direct leaf
writes to `76,001`, dropped leaf growth fast replacements to `30,199`, and
regressed the prepared insert step to `72.554 us/op`.
Merge fallback replacement counters now connect buffered fallback leaves to
later parent dirty-buffer coalescing. The current smoke profile reports
`future-current-header-partial-leaf` fallback replacement events of `5,324`
for `32-63` free-slot leaves, `5,895` for `64-127`, and `17,298` for `128+`;
the same rows flush with `2,715`, `1,641`, and `381` replaced pages
respectively. That makes the `32-63` regression actionable: a future behavior
slice needs conditional publication that preserves hot coalescing, not a wider
free-slot threshold.
Merge fallback parent-rank counters now classify fallback leaf admissions,
replacement events, and flush replacement states by the parent dirty buffer's
leaf page-id rank at merge time: no parent leaf, below the current parent max
leaf page id, or at/above that max. The current smoke profile reports almost
all `future-current-header-partial-leaf` admissions below the parent max leaf:
`18,348` of `18,349` `32-63` rows, `14,122` of `14,152` `64-127` rows, and
`1,725` of `1,983` `128+` rows. The small at/above-max `128+` class is still
hot, with `258` admissions but `13,218` replacement events. This makes the
next behavior slice a conditional-publication decision based on
admission-time parent context rather than another raw free-slot threshold.
Merge fallback tail-distance counters now split that broad below-max class by
page-id distance from the parent dirty-buffer leaf tail. The current smoke
profile reports `future-current-header-partial-leaf` admissions of `12,036`
in the `32-127` below-tail band and `20,215` in the `128+` below-tail band,
versus `1,944` closer than `32` pages below the tail. The small at/above-tail
class still admits only `289` rows but records `13,469` replacement events,
while the `128+` below-tail class records `10,793` replacement events. This
keeps the next publication work focused on a measured tail-distance and
replacement-state predicate rather than widening direct-write by rank alone.
A bounded below-tail direct-write experiment was not adopted: direct-writing
only `32-127` free-slot future-current leaves that were `32-127` pages below
the parent dirty-buffer leaf tail reduced dirty leaf pressure admissions to
`20,815`, but increased dirty leaf direct writes to `66,252` and regressed the
prepared insert step to `135.813 us/op`. A first version that added another
guard outcome also reproduced the embedded smoke `Can't initialize timers`
failure by expanding static TLS counter tensors, so future guard dimensions
need heap-backed counters or another TLS-neutral reporting path.
Merge guard/fallback counter tensors now use that heap-backed path. Test-hook
builds keep one lazy thread-local pointer for guard-outcome family and
leaf-shape counts, fallback replacement counts, parent-rank counts,
tail-distance counts, and flush replacement-state counts, reducing static TLS
pressure without changing direct-write policy. The storage-smoke embedded test
initializes successfully with this layout, and the prepared-insert benchmark
still reports populated guard/fallback tables with `76.765 us/op` step time,
`53,136` dirty leaf direct merge writes, and `34,484` dirty leaf pressure
admissions in that verification run.
Rejected below-tail direct-write candidate summary counters now expose the
specific failed predicate as a compact benchmark signal: `future-current-header-partial-leaf`
fallback rows with `32-127` free slots and a `32-127` page distance below the
parent dirty-buffer leaf tail. The current prepared-insert profile reports
`11,971` such admissions, `24` append replacements, `2,191` insert
replacements, and buffer-limit flush states of `11,538` never replaced, `185`
replaced once, and `238` replaced multiple times, while the broad dirty leaf
pressure and direct-write counts in that run were `34,484` and `53,136`.
Pressure-victim counters now join the same rejected below-tail candidates to
the page flushed by buffer-limit pressure before the slot is reused. The
current prepared-insert profile reports all `11,971` rejected-candidate victims
as checksum-dirty `index-leaf` pages, with `10,637` never replaced, `802`
replaced once, and `532` replaced multiple times before eviction.
Victim-shape counters now classify those same rejected-candidate pressure
victims by leaf fill, free slots, and page-id rank. The current profile reports
all `11,971` victims as `non-max-leaf-page-id`; `9,676` are `75-99%` full,
`2,292` are `50-74%`, and `3` are full, with victim free-slot detail
concentrated in `32-63` (`6,258`) and `64-127` (`5,172`).
The rejected-candidate free-slot matrix now compares incoming and victim leaf
capacity directly: `32-63` incoming candidates evict `3,704` `32-63` victims
and `1,698` `64-127` victims, while `64-127` incoming candidates evict `3,474`
`64-127` victims and `2,554` `32-63` victims. Only `87` candidate pressure
victims have fewer than `32` free slots.
The rejected-candidate replacement-state matrix now splits those same pressure
victims by admitted free-slot group and victim rewrite history. `32-63`
incoming candidates evict `4,862` never-replaced victims, `409` replaced-once
victims, and `356` replaced-multiple victims; `64-127` incoming candidates
evict `5,775` never-replaced victims, `393` replaced-once victims, and `176`
replaced-multiple victims. Most broad below-tail candidates therefore still
compete with broad never-replaced victims, so a behavior slice needs a stricter
predicate than admitted capacity plus victim replacement state.
The free-slot/replacement-state matrix now crosses the two victim dimensions
directly. Only `1,194` broad `32-127` free-slot victims were already replaced,
while `10,236` broad victims were never replaced and the below-`32` victim class
remained `87` rows. This narrows the next possible behavior experiment toward
preserving already-rewritten broad victims rather than bypassing the parent
dirty buffer for every broad below-tail incoming page.
That narrow behavior is now implemented as
`future-current-header-replaced-broad-victim-direct-write`: only a broad
future-current partial leaf that is `32-127` pages below the parent dirty-buffer
leaf tail and would evict an already-replaced broad victim bypasses fallback.
That slice's smoke profile reported `2,747` such dirty `index-leaf` direct writes,
with dirty leaf pressure admissions reduced to `31,979` and direct dirty leaf
merge writes increased to `55,902`; residual rejected-candidate pressure victims
no longer include already-replaced broad leaves. The sampled prepared insert step
was `80.670 us/op` under unrelated concurrent host build load, well below the
rejected broad below-tail experiment's `135.813 us/op` regression.
Preserved-victim matrix output now splits those `2,747` direct writes by the
incoming leaf and resident victim it protected. `32-63` incoming pages preserved
`421` replaced-once `32-63` victims, `663` replaced-multiple `32-63` victims,
`257` replaced-once `64-127` victims, and `210` replaced-multiple `64-127`
victims; `64-127` incoming pages preserved `412` replaced-once `32-63` victims,
`285` replaced-multiple `32-63` victims, `325` replaced-once `64-127` victims,
and `174` replaced-multiple `64-127` victims. That run reported a
`76.832 us/op` prepared insert step, `31,979` dirty leaf pressure admissions,
and `55,902` direct dirty leaf merge writes.
Preserved-victim lifecycle counters now report whether those protected resident
leaves later coalesce. The same profile records `1,152` first-time lifecycle
starts from the `2,747` direct-write decisions, `465` later replacement events,
and buffer-limit flushes for `1,138` tagged victims. Lifecycle starts are
`245` replaced-once and `184` replaced-multiple `32-63` victims behind `32-63`
incoming leaves, `133`/`87` `64-127` victims behind `32-63` incoming leaves,
`220`/`75` `32-63` victims behind `64-127` incoming leaves, and `145`/`63`
`64-127` victims behind `64-127` incoming leaves. The same sample reported a
`71.562 us/op` prepared insert step.
Lifecycle exit counters now close that accounting gap: the current
prepared-insert smoke profile reports `14` tagged preserved-victim discards and
`0` tagged clears, so the `1,152` lifecycle starts reconcile exactly as `1,138`
buffer-limit flushes plus `14` discards. Discards split as `5`/`2`
replaced-once/replaced-multiple `32-63` victims behind `32-63` incoming leaves,
`2`/`1` `64-127` victims behind `32-63` incoming leaves, `2`/`1` `32-63`
victims behind `64-127` incoming leaves, and `1` replaced-once `64-127` victim
behind a `64-127` incoming leaf. The same sample reported a `78.695 us/op`
prepared insert step.
Fallback-origin lifecycle exit counters now give the same non-flush accounting
for dirty-buffer merge fallback leaves. The current prepared-insert smoke
profile reports `6,634` residual rejected below-tail candidate admissions,
`6,627` buffer-limit flushes, `7` discards, and `0` clears, so those residual
candidate admissions reconcile exactly. The discarded residual candidates were
in the `32-127` below-tail band. The same sample reported a `78.120 us/op`
prepared insert step.
Dense broad-victim direct write now adds
`future-current-header-dense-broad-victim-direct-write`: when a `64-127`
free-slot below-tail incoming leaf would evict a checksum-dirty `32-63`
resident victim, merge direct-writes the incoming page and preserves the
denser victim in the parent dirty buffer. That prepared-insert profile
reports `2,773` such dirty `index-leaf` direct writes, reducing dirty leaf
pressure admissions to `28,551`, increasing direct dirty leaf merge writes to
`59,392`, and lowering index-leaf dirty refreshes to `87,944` without
reintroducing the rejected broad below-tail regression.
Equal broad-victim direct write now adds
`future-current-header-equal-broad-victim-direct-write`: when a `64-127`
free-slot below-tail incoming leaf would evict a checksum-dirty `64-127`
resident victim, merge direct-writes the incoming page and preserves the
equal-capacity victim in the parent dirty buffer. That prepared-insert
profile reports `2,113` such dirty `index-leaf` direct writes, reducing dirty
leaf pressure admissions to `26,199`, increasing direct dirty leaf merge writes
to `61,711`, and lowering index-leaf dirty refreshes to `87,911`. Residual
rejected below-tail candidate admissions fall to `4,461`, and the residual
`64-127` incoming / `64-127` victim matrix row is removed while broader
fallback leaves still coalesce in the parent dirty buffer.
Equal dense-victim direct write added
`future-current-header-equal-dense-victim-direct-write`: when a `32-63`
free-slot below-tail incoming leaf would evict a checksum-dirty `32-63`
resident victim, merge direct-writes the incoming page and preserves the
equal-density victim in the parent dirty buffer. That prepared-insert
profile reports `2,542` such dirty `index-leaf` direct writes, reducing dirty
leaf pressure admissions to `22,733`, increasing direct dirty leaf merge writes
to `64,611`, and lowering index-leaf dirty refreshes to `87,345`. Residual
rejected below-tail candidate admissions fall to `1,606`, and maintained-root
decode sites remain unchanged at `677` total.
Wider-victim direct write now adds
`future-current-header-wider-victim-direct-write`: when a `32-63` or `64-127`
free-slot below-tail incoming leaf would evict a checksum-dirty resident victim
with a strictly wider free-slot band, merge direct-writes the incoming page and
preserves the wider-capacity victim in the parent dirty buffer. The current
prepared-insert profile reports `2,119` such dirty `index-leaf` direct writes,
reducing dirty leaf pressure admissions to `21,031`, increasing direct dirty
leaf merge writes to `66,144`, lowering index-leaf dirty refreshes to
`87,176`, and lowering total zero-tail checksum calls to `227,063`. Residual
rejected below-tail candidate admissions fall to `121`, and maintained-root
decode sites remain unchanged at `677` total.
Dirty-page merge guard context now reuses the broad future-current leaf
decision state instead of recomputing incoming free slots, parent leaf-tail
distance, and would-be pressure victim through each predicate. The
prepared-insert smoke profile kept the same structural counters (`21,031`
dirty leaf pressure admissions, `66,144` dirty leaf merge direct writes,
`87,176` index-leaf dirty refreshes, and `677` maintained-root decodes) while
the sampled step time moved from `73.109 us/op` to `72.130 us/op`.
The merge guard now also classifies incoming future-current leaf fill once
before the full, near-full, `16-31`, and broad-victim predicates. Guard outcome
counts and checksum publication stayed unchanged (`122,388` future-current
guard rows, `66,144` dirty leaf merge direct writes, `21,031` pressure
admissions, `87,176` index-leaf dirty refreshes, `8` full-page checksum calls,
`227,063` zero-tail checksum calls, and `677` maintained-root decodes), and
the sampled storage-smoke prepared insert step was `70.928 us/op`.
Branch leaf-range redistribution and branch split writers now use the
prevalidated index-leaf pager writer for freshly encoded replacement leaves.
This preserves dirty-page undo, durable page bytes, active leaf-cache
publication, and dirty-buffer discard behavior while skipping the generic
branch-page publication probe for pages the caller already encoded as leaves.
The prepared-insert structural counters stayed unchanged (`24,796`
range-encoded leaves, `772` split-encoded leaves, `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `87,176` index-leaf dirty refreshes, and
`677` maintained-root decodes); timing samples on the shared host were noisy,
so this slice is tracked as a source-path simplification rather than a timing
claim.
Dirty-page pressure selection now folds the maximum resident leaf page-id scan
into the round-robin victim-selection pass while also keeping the best two
non-full dirty leaf fill candidates. The selector preserves the existing
clean-leaf, full-leaf, and high-fill non-max leaf priorities, leaves the same
prepared-insert structural counters (`21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, and `677`
maintained-root decodes), and moves the sampled storage-smoke prepared insert
step from `72.130 us/op` to `71.218 us/op`.
Packed row append-buffer first-page encoding now defers the initial checksum:
slot-`0` packed row pages created inside `write_packed_inline_insert_pages()`
start with a zero checksum and an append-buffer checksum-dirty flag, letting
append-buffer flush publish the checksum-valid durable page once. The current
prepared-insert profile removes `encode_packed_row_page` from the checksum
call-site table, lowers total zero-tail checksum calls to `227,063`, and lowers
insert-loop row zero-tail calls from `11,084` to `4,441`; maintained-root decode
sites remain unchanged at `677` total.
Packed index-entry append-buffer first-page encoding now applies the same
deferred checksum pattern to newly created packed index-entry pages. Reserved
append-buffer packed index-entry pages start with a zero checksum and a
checksum-dirty slot, while single-entry index pages and direct test helpers
keep immediate checksum encoding. The current prepared-insert structural
profile removes `encode_packed_index_entry_page` from the checksum call-site
table, lowers total zero-tail checksum calls to `227,063`, and lowers
insert-loop index-entry zero-tail calls from `224` to `18`; maintained-root
decode sites remain unchanged at `677` total.
Pressure eviction now prefers index leaves when a leaf is buffered, preserving
branch ancestors for repeated insert-loop rewrites.
When multiple leaves are buffered, pressure now evicts an already-checksummed
leaf before a checksum-dirty leaf, letting copy-for-read refreshes avoid later
flush-time checksum refreshes when possible.
When all buffered leaf candidates are still checksum-dirty, pressure now
prefers a structurally full leaf before a partially filled leaf, keeping hot
leaves that can still accept fitting same-leaf inserts resident longer.
Among dirty partial leaves, pressure now chooses the highest-fill valid
non-max page-id leaf before falling back to the original round-robin dirty
leaf. The max page-id exclusion is only a prepared-insert edge proxy, not a
durable index-ordering claim.
Dirty-page buffer flush profiles now classify flushed leaves by whether the
victim has the maximum page id among resident buffered leaves. The current VPS
prepared-insert smoke profile reports `50,579` buffer-limit leaf victims as
`non-max-leaf-page-id` and `3,418` as `max-leaf-page-id`, keeping follow-up
pressure changes grounded in edge-victim evidence instead of fill ratio alone.
The rank/fill-band cross-table shows `38,947` of the `75-99%` victims are
non-max page-id leaves, with `492` max page-id victims in the same band and
`2,194` max page-id victims in `50-74%`. With the high-fill non-max selector
enabled, the current smoke benchmark reports a `81.696 us/op` prepared insert
step, `85,529` non-max page-id buffer-limit leaf victims, and only `3` max
page-id victims, all in the full band.
Branch leaf-range redistribution now also preserves existing branch-refold
entryset caches by inserting the new logical row in sorted order instead of
forcing a later full branch leaf read or raw-order rebuild.
Insert cache retargeting now leaves active branch-refold entryset caches under
the insert plan's precise maintenance path, while fallback index-entry writes
still invalidate caches for the affected table/index pair.
Branch-refold planning now also inserts the current row into copied refold
caches in sorted order before snapshot publication, keeping same-insert refolds
on the direct leaf-preparation path. Branch refold fallback planning now
prechecks fixed-width root capacity before copying or rebuilding refold
entrysets, skipping impossible refolds without rereading branch leaves.
Branch snapshot preparation now writes leaf pages directly into the final
branch-plus-leaf page buffer instead of staging a separate leaf run and copying
it before publication.
Multi-page leaf encoders now consume their freshly zeroed output buffers
directly, preserving safe clearing for direct single-page callers while avoiding
redundant full-page clears during branch snapshot publication.
Branch snapshot preparation now derives child max keys and row ids directly from
the leaf encoder's ordered entry windows instead of rereading encoded leaf cells.
Full live-index tail reads now build row-id tracking lazily, allowing
append-only tails to extend branch refold entrysets without hashing the base
entryset.
Multi-level branch roots can now serve read-only exact, prefix, prefix-exists,
and full-index reads by recursively following lower branch pages. Eligible inserts
into packed full single-level branch roots now rewrite the root as a level-`2`
branch with two lower branch pages instead of publishing an append-tail
index-entry fallback.
Branch roots now persist their own total entry count while accepting legacy
zero-count branch pages through the catalog count fallback;
static no-tail exact point lookups against branch roots now read only the
selected target leaf after branch descent, leaving append-tail overlays on the
existing overlay-aware exact path; static no-tail exact entryset reads now
stream the selected branch key range without materializing the full branch leaf
list, and static no-tail prefix entryset reads now stream the selected prefix
range the same way. Static no-tail prefix-existence checks now stream the
selected prefix branch range without building the leaf run. Static no-tail full
entryset reads now stream branch leaves without first building the full
transient branch leaf list. Durable byte-safe forward range cursors can now
start from a prefix lower-bound suffix over published leaf and branch roots,
with append-tail overlay entries preserved for later handler positioning. Those
range cursors now keep row payloads lazy until MariaDB asks for a specific
cursor row, and MyLite reports coarse range estimates so simple bounded index
reads avoid ordered full-index scans when the optimizer can use range access.
Static no-tail published roots now bound forward range key-entry batches and
continue from the last emitted `(key, row_id)`, avoiding whole-suffix key
materialization for short range `LIMIT` reads. Roots with append-tail overlays
now keep that bounded static-root read path, eagerly scan the tail overlay, and
merge the live tail entries before returning the batch, leaving only long-tail
indexing for later cursor work.
insert overflow of a maintained single-page root now promotes fitting live
root-plus-tail entries to a stable single-level branch snapshot without a
catalog rewrite, while unsupported later branch-root row DML remains on the
append-tail overlay path. Fitting inserts into existing single-level branch roots now
rewrite the selected leaf and branch page directly when the child leaf has
space, and fitting leaf writes update the encoded page directly without
building and sorting a transient entryset, including high-key appends that raise
the final child fence while the last leaf has room. Full final child leaves can
now split into one appended leaf when the branch has child capacity and no
existing live tail overlay would be hidden by moving the branch tail; after
adjacent redistribution misses, the
same local split path now runs before broader bounded redistribution for any
selected full child while preserving branch order. Other full-leaf inserts
continue to use the append-tail fallback.
Active branch-root planning now caches verified branch-tail overlay checks on
the statement, so repeated split and redistribution decisions scan only newly
appended tail pages, and successful maintained branch inserts advance that
active cache through the final published page count for the maintained index.
The cache now retains a larger bounded set of branch shapes and evicts one
oldest entry at a time instead of clearing all cached tail ranges when broad
insert workloads exceed the limit. Nested statements now use the root active
cache owner, so prepared executions inside one transaction can reuse verified
tail ranges while nested rollback clears parent branch-tail caches
conservatively. Branch insert planning also keeps decoded leaf pages on the
root active statement and refreshes them from pager leaf writes, so repeated
same-checkpoint redistribution decisions do not reread or rechecksum unchanged
sibling leaves, and level-`2` branch insert planning can reuse descendant
leaves rewritten earlier in the same statement. Branch insert planning now also
advances root-owned branch-tail overlay caches after nested prepared row
executions, so successful maintained branch inserts extend or seed verified
non-overlay suffixes on the cache owner used by lookup. Fallback index-entry
publication now records concrete present-overlay cache entries on the same root
cache owner, and branch checks can reuse those entries across branch levels for
the same table, index, and key size when the cached overlay page is still after
the current branch tail, with present overlays taking precedence while absent
no-overlay ranges remain level-shaped.
Branch insert planning now also keeps decoded branch pages on the root active
statement for branch-root and level-`2` selected child branch reads, with pager
and buffered maintained
branch writes refreshing those cached fences. Branch snapshot leaf writes now
use a prevalidated pager path for freshly encoded leaf pages, refreshing active
leaf-page cache metadata without redecoding and rechecksumming those generated
pages. Branch snapshot root rewrites now use the maintained root/branch
dirty-page buffer, so active refolds can coalesce root publication with other
buffered root writes while fresh snapshot leaves stay on the existing
prevalidated leaf-write path.
Full final child inserts with live tail overlay can also refold the live
entryset into a fresh single-level branch snapshot when it still fits in one
branch page, reusing the planning-built refold entryset during execution.
Eligible final-child deletes now rewrite the
final leaf and branch fence when the branch child count stays stable, and
eligible final-child updates now rewrite the final leaf and refresh its branch
fence when the replacement entry stays in that final child. Eligible
same-child updates now also rewrite interior child leaves when the replacement
entry remains above the previous child fence and at or below the current child
fence. Eligible same-child deletes now also rewrite interior child leaves when
the child remains non-empty, with branch readers accepting non-empty underfull
children and branch inserts refolding when a later full-child insert can use
slack created by an earlier physical delete. Eligible cross-child updates now
move entries between existing child leaves when the source remains non-empty,
the target has room, and the branch child count stays stable, refreshing both
branch fences without appending a fallback index-entry page; stable child-count
cross-child updates that would empty the source child or overflow the target
now refold sorted live entries across the existing child pages under the
protected-page journal path. Eligible full-child inserts now split any existing
child leaf when the branch root has child
capacity and no live append-tail overlay would be hidden, inserting the
appended leaf's child cell in branch order, including branch roots with sibling
slack from prior redistribution or split maintenance; branch leaf-run readers
also preserve those non-packed child lists when live tail overlays force full
entry reads. When the single-level branch page itself is full and packed, the
same split can promote the root to a bounded level-`2` branch. Fitting inserts
into a level-`2` root's lower level-`1`
branch now rewrite the selected leaf, lower branch, and root branch directly
instead of publishing an append-tail index-entry fallback; full leaves under
that lower branch now split into one appended leaf when the lower branch has
child capacity and no live append-tail overlay would be hidden; packed full
lower branches under a level-`2` root can now split into one appended sibling
lower branch when the root has child capacity, and child-cell-full level-`2`
roots can now promote to bounded level-`3` roots for the same no-overlay insert
shape; fitting inserts below promoted level-`3` roots now refresh all three
branch levels without an append-tail fallback, and full leaves below that
level-`3` root can split when the lower level-`1` branch has child capacity
and no live append-tail overlay would be hidden; packed full lower branches
below that root can also split when the selected level-`2` child branch has
child capacity, and packed full level-`2` child branches can split when the
level-`3` root has child capacity; child-cell-full level-`3` roots can promote
to bounded level-`4` roots for the same no-overlay insert shape, and fitting
inserts below promoted level-`4` roots now refresh all four branch levels
without an append-tail fallback; full leaves below those roots can split when
the selected level-`1` branch has child capacity and no live append-tail overlay
would be hidden, and packed full level-`1` branches below those roots can split
when the selected level-`2` child branch has child capacity; packed full
level-`2` child branches below those roots can split when the selected level-`3`
child branch has child capacity; packed full level-`3` child branches below
those roots can split when the level-`4` root has child capacity; child-cell-full
level-`4` roots can promote to bounded level-`5` roots for the same no-overlay
insert shape. Fitting inserts below level-`5` and deeper roots now refresh the
selected branch path directly when the selected leaf has space and the dirty
path fits in the rollback journal; full leaves below those roots can now split
into one appended leaf when the selected level-`1` branch has child capacity
and no live append-tail overlay would be hidden, and packed full level-`1`
branches below those roots can now split when the selected level-`2` parent
branch has child capacity, while child-cell-full level-`2` child branches below
those roots can now split when the selected level-`3` parent branch has child
capacity, and child-cell-full level-`3` parent branches below those roots can
now split when the selected level-`4` parent branch has child capacity, and
child-cell-full level-`4` branches below those roots can now split when the
selected level-`5` parent branch has child capacity, while exactly full
level-`5` roots can now promote to bounded level-`6` roots, and child-cell-full
level-`5` branches can now split when the selected level-`6` parent branch has
child capacity, while exactly full level-`6` roots can now promote to bounded
level-`7` roots, and child-cell-full level-`6` branches below existing
level-`7` parents can now split while that parent has child capacity, including
under deeper roots, and exactly full level-`7` roots can now promote to bounded
level-`8` roots, and child-cell-full level-`7` branches below existing
level-`8` parents can now split while that parent has child capacity, and
exactly full level-`8` roots can now promote to bounded level-`9` roots, and
child-cell-full level-`8` branches below existing level-`9` parents can now
split while that parent has child capacity, while exactly full level-`9` roots
can now promote to bounded level-`10` roots, and child-cell-full level-`9`
branches below existing level-`10` parents can now split while that parent has
child capacity, while exactly full level-`10` roots can now promote to bounded
level-`11` roots, and child-cell-full level-`10` branches below existing
level-`11` parents can now split while that parent has child capacity;
broader recursive split-propagation cases remain on the append-tail fallback.
Eligible one-entry child removals now drop any branch child cell when deletion
reduces the expected child count by one and publish the removed leaf as a
one-page durable free-list run,
coalescing when the removed leaf is directly adjacent to the current free-list
root run. Eligible child-count-reducing deletes from multi-entry child leaves
now refold the branch into one fewer existing child page and reclaim the old
final child page when the refold remains journal-bounded; when a branch delete
reclaims the physical tail page, the following delete row-state write now
reuses that page instead of growing the file and publishing a free-list node.
Eligible child removals that leave a live entryset fitting one
maintained root page now collapse the branch root back to the maintained root
format when no live append-tail overlay would be hidden. Catalog page-run
allocation now reuses suitable non-root free-list runs, while catalog
reclamation and branch-leaf reclamation coalesce reclaimed runs with adjacent
runs anywhere in the linked free-list chain.
Broader file shrinking/free-space compaction and broader branch update/delete
maintenance remain pending. SQL copy-rebuild DDL now
opportunistically publishes those roots for all current supported fixed-width
keys in rebuilt tables, including retained primary keys after forced copy
rebuilds, with one shared append-history scan and one catalog publication for
the raw key set. Active checkpoint
write amortization now reuses one statement recovery journal, defers header
publication to checkpoint boundaries, and caches guarded exact duplicate-key
probes on the outer active checkpoint across nested libmylite statement
checkpoints. This materially improves fixed-width index publication
and routed indexed insert throughput. Maintained branch inserts now stage
repeated existing root and branch routing-page rewrites in the dirty page
buffer while keeping leaf rewrites on the immediate write path. Dirty buffered
maintained branch/root pages can now defer checksum publication until generic
dirty-buffer reads or flush, avoiding redundant branch checksum refreshes on
hot fitting insert loops. The same checksum-dirty dirty-buffer contract now
covers maintained index leaf pages for simple branch-leaf inserts, so repeated
active rewrites can refresh leaf checksums at copy or flush boundaries instead
of before each buffered write. Branch snapshot publication now writes the
contiguous fresh leaf run with one direct file write while keeping the existing
branch root on the dirty-page buffer path. Planned branch-refold snapshots now
normalize non-cache raw entrysets before publication and then skip the encoder's
raw-entry order probe for the same row insert. Branch snapshot root encoding
now derives child ids and max fences directly from the freshly encoded leaf run
instead of allocating temporary child-fence arrays. Single-level
branch insert maintenance now redistributes a full selected leaf with an
adjacent sibling leaf when the branch has total slack, preserving the existing
child count instead of refolding the whole branch root for that local insert
shape. That redistribution now extends to the nearest bounded contiguous leaf
range with slack, covering cases where the selected leaf and immediate sibling
are full but a nearby branch child has room while still fitting the journal
protected-page budget; the planner scans each candidate leaf at most once per
direction and leaves execution-time page rereads on the rollback-protected
rewrite path. This static leaf-range redistribution also remains available when
the branch has a live append-tail overlay, preserving the overlay through the
overlay-aware read path instead of forcing a full branch refold. Single-level
branch maintenance now also splits the selected full leaf before broad bounded
range redistribution when adjacent redistribution misses, the branch has child
capacity, and no live tail overlay would be hidden.
Level-`2` branch roots now also redistribute a bounded leaf range inside the
selected lower branch when the full selected leaf can be absorbed by sibling
slack without appending a new static page, then refresh the parent root branch
fence while preserving any existing append-tail overlay.
Live-overlay branch refolds now carry the planning-built entryset into the
writer, avoiding a second branch-root entryset read for that same row insert.
Successful refold inserts also refresh a bounded active statement refold
entryset cache, letting the next matching same-statement refold start from the
post-refold entryset instead of rebuilding it from branch leaves. Whole-branch
refold planning now also enforces a fixed leaf-page budget before copying or
rebuilding the refold entryset, leaving larger live-overlay cleanups on the
append-tail fallback until localized branch maintenance can absorb them.
Active index page cache stores now replace or append leaf and branch pages
after one cache lookup, avoiding repeated linear probes on maintained insert
and refold page writes.
Active index page cache refresh after trusted pager writes now reads leaf and
branch page metadata directly instead of running full checksum-validating
decoders just to update transient statement-local caches.
Packed insert eligibility now uses a branch-cache hit probe when it only needs
to know that a branch root is already cached, avoiding a full cached branch
page copy in that read-only decision.
Packed index-entry tail validation now remembers already checked active
append-buffer tail ranges per cache entry and rescans only after new pages or
buffered-page rewrites make that range incomplete or stale.
Branch leaf-range planning now also probes active leaf-cache metadata directly
when it only needs sibling leaf entry counts, avoiding a full cached-page copy
on repeated same-statement redistribution checks.
Selected-leaf branch insert planning now uses the same metadata-only cache
probe before falling back to a full page read, avoiding cached leaf-page copies
when planning only needs entry count and key shape.
Branch leaf-range planning now resolves the active leaf-cache handle once per
range planning attempt and reuses it across left/right sibling scans.
Branch insert planning now carries the selected child offset out of the same
branch descent that finds the child page id, avoiding a second child-cell scan
before redistribution or branch-fence validation.
Single-leaf branch refresh now consumes that planned child offset when
available, validates the stored page id before mutating the branch cell, and
reports offset hits versus scan fallbacks in prepared-insert benchmark output.
Leftward branch leaf-range planning now writes candidate leaf ids into a
prepositioned buffer slice instead of prepending with repeated `memmove()`
calls while preserving the writer's ascending range order.
Single-level maintained branch insert writers now reuse active statement leaf
and branch page caches populated by planning before falling back to pager reads
and durable decoders.
Level-two maintained branch insert writers now reuse the same active branch
and leaf page cache helpers for their root, selected child branch, and selected
leaf reads before falling back to pager reads and durable decoders, with
benchmark counters separating helper cache hits from fallback decodes.
Single-child branch refresh after a leaf insert now validates the changed child
fence against neighboring cells instead of fully decoding and re-checksumming
the just-mutated branch page.
Leaf-range redistribution branch refresh now applies the same targeted
entry-count and child-fence validation after updating range child maxima,
avoiding another full branch decode on the prepared insert path.
Active checkpoint and snapshot header reads now reuse the decoded in-memory
header instead of re-encoding and
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
Prefix-existence fallback checks now use an allocation-free row-id overlay,
avoiding full or narrowed key-entryset materialization when static no-tail leaf
roots cannot answer directly. Static no-tail prefix-existence checks now start
from the first leaf page or branch child range whose last key is not below the
requested prefix.
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
Integer exact-key direct updates now skip the eager nullable-value probe before
building the key image, letting the integer store evaluate bound key values once
while preserving non-integer `NULL` no-match behavior.
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
Successful perf-harness runs now also print the final `.mylite` file bytes,
header page size, and header page count after close, making storage write
amplification visible alongside step and commit timings.
The direct row materialization boundary now decodes legacy row references
through explicit page-id and slot helpers, keeping today's row-id-as-page-id
encoding unchanged while preparing the packed-row format work to stop treating
every row reference as a physical page id.
Packed-row references now reserve the high bit as a marker with 51 physical
page-id bits and 12 slot bits; marked references are recognized and rejected by
legacy row materialization until packed row pages exist.
Fixed-size inline packed row pages can now be read through marked slot
references: scans, counts, direct reads, and row-state deletes treat each slot
as one opaque row id, while production row writers still emit legacy one-row
pages until packed write, index-entry, and recovery behavior is covered.
Index-entry, maintained-root, and published-leaf readers now validate stored
row ids as opaque row references, and exact indexed lookup can materialize a
packed slot from an append-only index entry while stale packed entries are
filtered through row-state visibility.
Packed row pages now use row-page version `2`, so even the first row on a
packed page returns a marked slot-`0` row reference without aliasing the
legacy unmarked physical page id. Row-page version `1` remains restricted to
legacy one-row pages.
Active no-index fixed-size inserts now pack multiple rows into one buffered
version-`2` row page, including nested-savepoint rollback coverage for slots
added after the savepoint. Indexed inserts and oversized rows remain on the
legacy writer until the next packed append slices thread marked row ids through
index publication.
Active append-only indexed fixed-size inserts now also pack rows and publish
append-only index-entry pages with marked packed row references; exact indexed
lookup materializes those packed slots before and after commit, and stale
packed index entries are filtered after delete. Those append-only entries can
now share version-`2` table-index pages when table id, index number, and key
size match, and the active writer now keeps a bounded per-shape append cache so
multi-index inserts can continue packing each index shape across later row pages
and other-shape append-tail pages without changing row-reference semantics.
Packed index-entry tail-validation memos now also advance across writer-known
compatible row pages and other-shape index pages, avoiding rescans of pages the
writer just appended.
Active indexed inserts into in-place writable
single-page maintained roots can now also publish marked packed row references
through maintained-root cells while preserving exact lookup before and after
commit. Active fixed-size inserts can now also keep using marked packed row
references after maintained roots overflow or promote to branch roots. The
maintained-index planner replans when branch/root planning changes the fallback
append-page count, keeping duplicate-key branch leaf selection aligned with the
final packed or legacy row reference.
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
Prepared inserts now have the same focused component split, so the remaining
insert hot path can be separated into bind, step, and reset costs before the
next insert-specific optimization pass.
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
`ColumnValue` vectors on each cache hit. Cache-served rows now go one step
further by publishing the cached entry directly to public column accessors,
avoiding per-hit row-value copies while preserving normal statement-owned
values for MariaDB-produced rows. Cached-row replay plus cached
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
The same one-row result-cache classifier now also accepts a narrow identifier-only
range-min shape, letting hot recurring prepared
`WHERE key >= ? ORDER BY key, id LIMIT 1` workloads replay the MariaDB-produced
row while the retained read scope remains valid.
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
Stable child-count branch update refolds now cover source-emptying and
target-full cross-child updates when the existing branch children fit the
protected-page journal bound. Branch deletes that reclaim the current physical
tail page now reuse it for the required delete row-state page. Broader
transactional maintained index mutation remains planned.
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
those rollback preimages. Same-size row-only and single changed-index buffered
rewrites now capture only the row payload or key bytes they mutate, leaving the
checksum dirty for later refresh instead of copying unchanged metadata into
rollback preimages. Successful row-DML statement cleanup can also retain
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
work. Active in-place rewrites that are already covered by a statement-chain
durable-cache retarget marker now skip recording redundant nested markers, while
header-advancing mutations keep the existing marker refresh path. Nested
statements now cache inherited durable-cache retarget marker coverage when they
start, so repeated covered row-only rewrites avoid parent-chain marker walks
while ambiguous multi-table inheritance keeps exact parent-chain semantics.
Cached-shape
buffered row rewrites with unchanged index entries now skip
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
Changed-index active replacement rows now also cache row-specific empty
maintained-root update plans for the active catalog generation, so repeated
prepared updates that keep rewriting the same buffered row id skip redundant
maintained-root planning while preserving-index row-only updates still run the
retarget planner.
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
Indexed-row payload lookup now repeats the active exact-index cache probe
before entering the miss-capable row-id helper, so steady prepared point-update
reads can return from the active cache without the larger lookup frame.
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
at their cache-probe and exact-entryset materialization call sites.
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
Eligible exact-key direct updates that preserve all index entries now snapshot
only compact fixed-integer updated fields for no-op detection and write the new
row through the preserving-index storage path, avoiding full old-record copying
and the nested generic handler update path while unsupported shapes keep the
existing fallback.
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
Active row-payload cache reads now remember the last successful bucket, letting
the immediate same-row update validation reuse that bucket before falling back
to the normal row-id hash probe.
Row-update execution now also defers active live-row cache lookup until
payload-cache validation misses or a successful update actually replaces the
row id, avoiding a no-op cache probe for steady row-only rewrites.
Row-payload cache hit checks are now hot-inlined in indexed-row materialization
and row-id batch reuse, removing a tiny helper call from repeated cache hits.
Indexed-row active payload cache hits now keep the uncached row-page read and
its scratch page buffer in a no-inline slow helper, avoiding the large stack
probe on repeated cache-hit materialization.
Exact indexed-row materialization now also copies proven active row-payload
cache hits directly from `find_indexed_row_payload_with_header()`, bypassing
the generic materializer on the steady prepared update path.
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
index-root planning did not allocate a local catalog image. Preserved-index
row-only storage updates now skip maintained index-root update-plan setup,
empty-plan checks, no-op maintained-root writers, and plan cleanup entirely.
Handler paths that need both stable filename and table-name identities now enter
one combined storage identity scope instead of separate filename and table-name
scope calls.
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
Prepared insert storage now reuses active cache and append-buffer ownership
derived from the update scope for inline append-page reservation and live-row
cache maintenance instead of rediscovering the active chain inside each helper.
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
measures about 1.31 us/op for the step component over 10000 rows / 1000000
iterations after key-changing exact-key updates enter the rebind shortcut. A
row-only expression phase,
`prepared-row-only-update-components`, keeps the same prepared exact-key SQL
path but updates a non-indexed integer `counter` column in a separate table,
separating SQL-layer prepared update overhead from secondary-index replacement
cost. A companion `prepared-row-only-update-miss-components` phase binds
out-of-range primary keys against the same row-only table and records a zero
checksum, isolating table-open, prepare, lock, exact lookup, and reset cost
from row materialization and storage mutation; a local 10000-row /
1000000-iteration sample measured the no-match step at about 0.89 us/op after
the row-only shortcut. Prepared-DML execution reuse is tracked in
[Prepared DML execution reuse](specs/prepared-dml-execution-reuse/specs.md),
with the direct-update rebind subset detailed in
[Prepared direct update rebind](specs/prepared-direct-update-rebind/specs.md).
The implemented shortcut keeps table opening and locking per execution, then
rebinds the freshly opened table state before bypassing repeated
`JOIN::prepare()` for the already-proven exact-key MyLite direct-update subset.
The embedded storage-engine regression covers repeated prepared exact-key
updates across strict conversion errors, CHECK failures, generated-column index
maintenance, transaction rollback, metadata reprepare, and same-name
temporary-table shadowing.
`Sql_cmd_update` now records the exact MyLite direct-update shape accepted by
the normal path for embedded prepared statements, giving the rebind work an
explicit cache to validate against freshly opened tables instead of relying on
transient range-planner state.
That shape cache now records a key-field fingerprint and can validate it
against the freshly opened table and MariaDB table reference before retargeting
the SQL-layer exact-key proof cache to a copied prepared-condition tree. The
direct execution shortcut is now enabled for exact-key prepared updates whose
condition is fully guaranteed by the key: it still runs MariaDB's table open
and MDL/external-lock lifecycle, but skips repeated `JOIN::prepare()` after a
fail-closed fresh-table rebind boundary instead of retaining stale `TABLE`,
`JOIN`, or range-planner state. Stable repeated executions retain the cached
fingerprint when the current accepted proof still targets the same MariaDB
table reference, avoiding another key-shape walk in the hot path. Key-changing
updates also use the shortcut after the first normal execution has accepted
MyLite's direct update contract, while generated/virtual-column,
metadata-stale, and unsupported observable paths keep the normal MariaDB route.
The MyLite handler now mirrors InnoDB by advertising zero SQL-layer
`THR_LOCK` rows; MariaDB still calls `store_lock()` and `external_lock()`, while
MDL plus MyLite file and checkpoint locks own storage correctness. Local
samples after that lock-row optimization measured prepared row-only update
steps at 1.191 us/op and assignment update steps at 1.248 us/op over 10000
rows / 10000000 iterations; 1000000-iteration samples measured row-only misses
at 0.867 us/op and expression key-changing updates at 1.358 us/op.
The first implementation step caches the immutable prepared-update value-list
subquery shape on `Sql_cmd_update`, avoiding repeated value-list scans before
the MyLite single-update result-elision gate. `Sql_cmd_update` now also caches
the immutable simple-value setup
classification, avoiding another assignment-list walk on repeated prepared
executions. The remaining unsupported prepared-update benchmarks still show
repeated table-open, value-expression setup, and `JOIN::prepare()` work outside
the guarded exact-key shortcut. The MyLite handler now also caches accepted
non-key direct-update shape facts for prepared statements, avoiding repeated
handler-side metadata walks for stable row-only updates while key-changing
updates keep the existing uncached FK-sensitive path. Accepted integer-key
direct updates now also serialize bound integer lookup keys through a guarded
handler-local `Field::store()` path instead of the generic
`Item::save_in_field()` dispatcher, while non-integer predicate values keep the
existing MariaDB
conversion path. The same integer-key path now skips generic key-field reset
and key-buffer zeroing before the fixed-width integer store, while text-bound
key predicates keep the existing reset plus generic conversion path. Accepted
direct updates now hoist the stable filename and table identity scopes across
the exact read plus update mutation, so the inner
storage read and write helpers no longer establish duplicate identical scopes
for the same direct-update operation. Direct-update exact row reads also skip
clearing handler cursor buffers when the caller explicitly requests no cursor
state publication, while ordinary indexed reads keep the existing cursor reset
and continuation behavior. Direct row-only updates that already proved stable
indexed key images now skip per-row foreign-key presence probes, while
key-changing direct updates and ordinary row updates keep the existing FK
validation path.
Stable row-only direct updates now also cache their compact old-value snapshot
field indexes, record offsets, and byte lengths through the handler
direct-update shape cache, so row execution copies and compares cached record
buffer slices instead of walking the update-field item list for no-op
affected-row detection.
Direct-update shape-cache hits now return before clearing compact snapshot
state in `direct_update_rows_init()`, leaving cache misses on the existing
reset and recompute path while avoiding redundant state zeroing on stable
prepared row-only updates.
A VPS syscall profile of no-match prepared row-only updates shows the remaining
large gap is in repeated MariaDB table/file open and execution-envelope work,
not MyLite storage mutation; storage indexed-row update mutation measured only
about 2.229 us/op in the same environment. A follow-up filtered stack trace
identified the largest repeated procfs component as `THD::store_globals()`
rediscovering stack bounds through `pthread_getattr_np()` on each embedded
command; the embedded stack-bounds cache now reuses same-thread bounds while
retaining rediscovery for moved THDs. On the VPS, no-match prepared row-only
update step time dropped from `244.372 us/op` to `9.961 us/op` over
`1000 x 100000`, and the same 20-iteration short trace dropped
`/proc/self/maps` opens from 3052 to 2. The same-thread embedded thread-id
cache now reuses `os_thread_id` while `pthread_self()` is unchanged; a 10k
no-match trace dropped `gettid` from 13032 calls to 2, and the 100k no-match
step measured `8.080 us/op`.

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
