# Compatibility Harness

MyLite compatibility evidence is grouped by product surface. Raw CTest presets
are still useful, but compatibility work should name the behavior it proves:
public API, storage format, recovery, locking, embedded lifecycle, sidecar
lifecycle, storage-engine routing, and routed SQL.

## Runner

Use the first-party harness for grouped local runs:

```sh
tools/mylite-compat-harness list
tools/mylite-compat-harness run public-api storage-core
tools/mylite-compat-harness report embedded-lifecycle sidecar routed-ddl-dml
```

With no group arguments, `run` and `report` execute all implemented groups.
Reports are written to `build/mylite-compat-report.md` unless
`MYLITE_COMPAT_REPORT` points somewhere else.

The harness maps each group to a CMake preset and a CTest label. Groups that
need MariaDB embedded artifacts prepare those artifacts before configuring the
first-party preset. Storage-engine groups use the opt-in
`build/mariadb-mylite-storage-smoke` archive with `PLUGIN_MYLITE_SE=STATIC`.

## Current Groups

| Group | Preset | CTest label | Surface |
| --- | --- | --- | --- |
| `public-api` | `dev` | `compat-public-api` | C API argument validation and version surface |
| `storage-core` | `dev` | `compat-storage` | Primary file, catalog, rows, indexes, cross-process read snapshots, and mutations |
| `crash-recovery` | `dev` | `compat-crash-recovery` | Rollback-journal, transaction-journal, and corrupt-journal recovery |
| `transaction` | `storage-smoke-dev` | `compat-transaction` | Direct and prepared row-DML transaction start/completion, transactional engine table flags, two-handle transaction-owner read snapshots, cross-process transaction-journal read snapshots, MEMORY/HEAP volatile transaction/savepoint rollback, generated, explicit-insert/update, and `INSERT ... VALUES` / `INSERT ... SELECT` `ON DUPLICATE KEY UPDATE` explicit-update autoincrement rollback and reservation gaps, supported direct and prepared session autocommit mode, read-only/read-write, isolation-level, transaction variable, temporary row-DML, and completion-type transaction SET controls, direct and prepared case-insensitive quoted savepoint rollback/release, active transaction crash recovery, and unsupported transaction-control policy |
| `transaction-hooks` | `storage-smoke-dev` | `compat-transaction-hooks` | MariaDB statement transaction and handler savepoint hook integration, including representative MEMORY volatile failed-statement and savepoint rollback |
| `statement-rollback` | `storage-smoke-dev` | `compat-statement-rollback` | Failed statement rollback for routed MyLite storage, including prepared row-DML, failed-DML autoincrement generated reservation, source-driven insert-select reservation growth, failed first-key ODKU reservation preservation, grouped ODKU source-read, update-expression, generated-expression, and CHECK-constraint rollback plus mixed grouped failed-DML and UPDATE IGNORE matrices to live prefix maxima, prior-success update preservation across representative FK, duplicate-key, and CHECK failures, explicit duplicate non-consumption, and update gaps, representative mixed-row autoincrement matrices, representative row `REPLACE`, OR REPLACE replacement, failed copy ALTER conversion, and table-DDL failures |
| `partition` | `storage-smoke-dev` | `compat-partition` | Partition DDL and partition-management rejection until partition metadata and routing exist |
| `foreign-key` | `storage-smoke-dev` | `compat-foreign-key` | Foreign-key DDL publication, metadata, row checks, bounded actions, and unsupported-action policy coverage |
| `check-constraint` | `storage-smoke-dev` | `compat-check-constraint` | CHECK constraint enforcement, ALTER existence-option skips, CTAS targets, representative grouped ODKU CHECK rollback, and failed ADD CHECK rollback on routed tables |
| `generated-column` | `storage-smoke-dev` | `compat-generated-column` | Generated column read/write, CTAS projection, ALTER, BLOB/TEXT prefix, generated-column index, and representative DML and expression-error rollback coverage |
| `unsupported-index` | `storage-smoke-dev` | `compat-unsupported-index` | Unsupported FULLTEXT, SPATIAL, vector, and long-unique index rejection |
| `locking` | `storage-smoke-dev` | `compat-locking` | Primary-file lock conflicts, busy timeouts, and SQL locking policy |
| `embedded-lifecycle` | `embedded-dev` | `compat-embedded-lifecycle` | Embedded runtime open/close, direct execution, and cleanup |
| `direct-sql` | `embedded-dev` | `compat-direct-sql` | `mylite_exec()` SQL execution and statement effects, including representative ODKU insert-id behavior |
| `prepared-statement` | `embedded-dev` | `compat-prepared-statement` | Prepared statements, typed bindings, binary-safe column access, and statement effects, including representative ODKU insert-id behavior |
| `column-metadata` | `embedded-dev` | `compat-column-metadata` | Prepared statement column metadata |
| `large-value` | `embedded-dev` | `compat-large-value` | Prepared statement large-value segment reads |
| `warning` | `embedded-dev` | `compat-warning` | Warning counts and structured `SHOW WARNINGS` rows |
| `sql-comparison` | `embedded-dev` | `compat-sql-comparison` | MariaDB baseline SQL API comparison, including direct, prepared, and typed prepared expression result sets |
| `storage-engine` | `storage-smoke-dev` | `compat-storage-engine` | Static handler registration, routed engine metadata, BLACKHOLE row-discard smoke, and MEMORY/HEAP volatile-row smoke including statement and transaction snapshots |
| `sidecar` | `storage-smoke-dev` | `compat-sidecar` | Forbidden durable sidecar gates |
| `routed-ddl-dml` | `storage-smoke-dev` | `compat-routed-ddl-dml` | Routed direct/prepared schema namespaces, directory-free file-backed `CREATE DATABASE`, catalog-backed `CREATE DATABASE` existence options, table and index DDL, ordinary `CREATE TABLE IF NOT EXISTS`, column ALTER existence-option skips, index DDL existence-option skips, index rename existence-option skips, index ignorability, non-CHECK primary/unique constraint DDL including primary-key add/drop/re-add, unique key-name matrices, failed ADD UNIQUE rollback, and failed strict conversion ALTER rollback, CHECK constraint existence-option skips, indexed table/index rename, table-DDL `IF EXISTS` skips, failed table-DDL rollback, temporary LIKE/CTAS isolation, shadowing, and OR REPLACE, online/in-place ALTER rejection, `CREATE TABLE ... SELECT` duplicate modes, plain/LIKE/CTAS OR REPLACE, truncate, DML, ODKU statement-effect smoke, and direct/prepared grouped later-in-key ODKU smoke |
| `application-schema` | `storage-smoke-dev` | `compat-application-schema` | WordPress-shaped core schema, WordPress installer DDL and seed fixtures including deterministic default options and role payloads, WordPress multisite schema/seed fixtures, BuddyPress component schema/seed fixtures, Laravel, Django, and Rails Active Record schema fixtures, expanded collation restart matrix, and prefix-index smoke |
| `server-surface` | `storage-smoke-dev` | `compat-server-surface` | Unsupported server, binlog/replication, replication/binlog filter assignment, binlog/replication system-variable assignment and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL file-I/O, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection including known external no-equals names, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, routine debug-code inspection, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, view runtime, stored-program runtime, trigger runtime, dynamic UDF runtime, and non-table object surface policy |

## Relationship To MariaDB MTR

The `sql-comparison` group compares representative direct execution, direct,
prepared, and typed prepared expression result sets, prepared statement,
metadata, and warning behavior against a raw MariaDB embedded runtime.
MariaDB's MTR remains the long-term source for broad upstream compatibility
cases. MyLite should not run MTR
blindly as the primary local
signal yet: current MyLite behavior is embedded, file-owned, and intentionally
excludes server surfaces that many upstream suites assume. The opt-in
`tools/mylite-mtr-harness` runner proves the embedded MTR path with a curated
smoke list covering the MyLite trimmed bootstrap schema, upstream scalar
CAST/CONVERT, CASE-family and ANSI SQL-mode expression, selected numeric,
integer, varchar, binary-string, binary/national character, date/timezone,
leap-second timezone, temporal literal, system-timezone,
parser/comment, comparison, selected negation-elimination predicate optimizer
behavior, selected boolean aggregate/HAVING expression behavior, selected
subquery and subquery `ANALYZE FORMAT=JSON` behavior, DDL/comment metadata,
selected MTR-profile view, trigger, and stored-procedure DDL/runtime
behavior, DDL constraint/index metadata, selected Aria ALTER/index-upgrade
behavior, selected lock-table DDL behavior, deprecated server syntax rejection,
selected embedded-profile native-engine absence,
selected embedded-profile disabled diagnostic behavior,
selected embedded-profile disabled metadata behavior,
selected embedded-profile host-file SQL I/O rejection,
selected embedded-profile optional SQL function absence,
selected embedded-profile dynamic-column disabled fallback behavior,
selected embedded-profile disabled SQL-surface behavior,
`ORDER BY`, optimizer-cost metadata, selected EXPLAIN plan output,
selected Aria range, semijoin, and rowid-filter optimizer behavior, selected
UNION, EXCEPT / EXCEPT ALL, INTERSECT, and mixed set-operation behavior,
selected compound-statement parser diagnostics, selected missing-routine
diagnostics, SIGNAL/RESIGNAL diagnostics, row constructors, selected scalar
`LAST_VALUE()`, window `FIRST_VALUE()` /
`LAST_VALUE()`, percentile/median,
and window-function behavior, selected `IF()` / `NULLIF()` conditional
expressions, selected SET-family scalar functions, scalar operator,
string/format, charset-conversion expressions, crypto/KDF, disabled DES,
aggregate DISTINCT and indexed count-distinct, BIT-key autoincrement, autoincrement ODKU,
strict HEAP autoincrement,
temporal scale, high-resolution temporal functions, microsecond parsing,
date-format, ASCII charset, selected
Latin2, UTF-8 binary/general, UTF-8 UCA 1400, and utf8mb3 general-1400
charset, weight-string, and LIKE condition-propagation behavior, and multibyte
charset recoding, plus selected charset CREATE/ALTER inheritance and conversion
diagnostics, selected locale formatting behavior, and selected support-tool and
mysqltest command behavior.
It uses a separate
`build/mariadb-mtr-smoke`
profile because the default embedded profile intentionally omits view, stored
program, trigger, and binlog sysvar surfaces that MTR bootstrap still expects.
The curated list also covers selected filesystem charset,
UTF-32 `character_set_collations`, JSON equality/normalization behavior, and
raw embedded dynamic-column disabled fallback behavior, plus selected ODBC
compatibility syntax, optimizer-trace default metadata, SHOW row-order, system
`mysql` table reference, long-tmpdir view, selected slow-log variable and
general-log path state, deprecated rename-database diagnostic behavior, and
selected system-variable charset/collation, cache/limit, capability, metadata,
function-style mutation, session-control, security, local-infile, and
default/version behavior, plus selected `my_print_defaults`, `mysqltest`, and
`perror` support-tool behavior.
The same harness also exposes a separate storage-routed MTR mode with
`list-storage`, `run-storage`, and `probe-storage`. That mode uses
`build/mariadb-mtr-storage-smoke`, enables the static MyLite storage engine only
for MyLite-owned storage tests, prepares a fresh primary `.mylite` file, and
currently covers selected explicit MyLite and engine-alias routing through
`mylite.routed_storage_engines` plus MyLite-owned schema sidecar absence
through `mylite.routed_storage_sidecars`, plus raw catalog-backed schema
lifecycle coverage for same-name routed tables, duplicate
`CREATE TABLE IF NOT EXISTS`, missing-target `CREATE TABLE IF NOT EXISTS`,
mixed `RENAME TABLE IF EXISTS`, and `DROP TABLE IF EXISTS` through
`mylite.routed_storage_schema_lifecycle`, and routed `InnoDB` transaction,
explicit MyLite transaction, rollback, commit, and savepoint behavior through
`mylite.routed_storage_transactions`, plus representative routed `InnoDB`
and explicit MyLite foreign-key publication and enforcement through
`mylite.routed_storage_foreign_keys`, and representative routed `InnoDB` CHECK
and explicit MyLite CHECK and generated-column metadata, enforcement, generated
values, generated-index reads, and generated unique-key diagnostics through
`mylite.routed_storage_constraints`, plus representative routed `InnoDB` DDL
lifecycle behavior plus explicit MyLite DDL lifecycle behavior for
`CREATE TABLE ... LIKE`, CTAS, representative successful
`CREATE OR REPLACE TABLE` plus pre-drop missing-source replacement failure,
copy `ALTER`, indexed reads after rebuild, and `RENAME TABLE` through
`mylite.routed_storage_ddl_lifecycle`, plus representative routed `InnoDB`
DML statement effects plus explicit MyLite DML statement effects for ODKU,
`REPLACE`, keyed `UPDATE`, keyed `DELETE`, affected rows, insert ids, and
final indexed visibility through `mylite.routed_storage_dml_effects`, plus raw
large `TEXT` / `BLOB` payload reads, bounded prefix-index forced reads,
large-value update filtering, and unique `TEXT` prefix rejection through
`mylite.routed_storage_large_values`, and
representative routed `InnoDB` plus explicit MyLite autoincrement generated
ids, explicit high-value advancement, rollback gaps, offset/increment values,
and truncate reset through `mylite.routed_storage_autoincrement`, plus
representative unsupported engine request rejection and failed table metadata
absence through `mylite.routed_storage_unsupported_engines`, and representative
unsupported FULLTEXT, SPATIAL, vector, and long-unique index rejection through
`mylite.routed_storage_unsupported_indexes`, plus representative raw
MEMORY/HEAP volatile transaction, savepoint, failed-statement rollback, and
autoincrement gap behavior through
`mylite.routed_storage_volatile_transactions`, plus representative raw
partition-definition rejection and failed table metadata absence through
`mylite.routed_storage_partitions`, plus representative raw online and
in-place ALTER rejection, copy-ALTER preservation, blocked-column metadata
absence, and sidecar absence through `mylite.routed_storage_online_alter`,
plus representative raw temporary LIKE, CTAS, same-name shadowing, and
post-drop metadata cleanup through `mylite.routed_storage_temporary_tables`,
plus representative raw ignored secondary-index metadata, forced-hint
rejection, copy-ALTER toggling, and `SHOW CREATE TABLE` output through
`mylite.routed_storage_ignored_indexes`.
It remains outside the default compatibility groups because it builds
`mariadbd` and several upstream client/support tools. Broader MTR integration
should be a separate comparison slice with explicit include lists, expected
unsupported surfaces, and stable result normalization.

`tools/mylite-mtr-harness coverage` reports the accepted curated MTR count
against the imported test-file inventory without configuring, building, or
running MTR. The current inventory contains 5,901 imported upstream MTR test
files plus 27 MyLite-owned MTR files; accepted coverage is 413 upstream baseline
tests, 8 MyLite profile tests, and 19 MyLite storage-routed tests. The known
unsupported inventory currently records 3,091 upstream MTR files that are
intentionally outside accepted coverage because exact probes or suite selectors
show they require disabled embedded, native-engine, binlog, replication/Galera,
Performance Schema, server plugin, sys schema, native encryption, partition,
Oracle SQL mode, temporal table, Sequence, log-table, or profile-specific
result surfaces. This
is a scale measurement, not compatibility proof for unrun tests.
Probe summaries distinguish passed, failed, and skipped candidates; skipped
tests remain non-coverage but are kept separate from runtime or result
failures during discovery. `tools/mylite-mtr-harness list-unsupported` prints
known non-coverage probes and selector-expanded suite rows with reason
categories so future MTR expansion can avoid rediscovering the same unsupported
surfaces, while `list-unclassified` prints the remaining imported upstream
tests that still need acceptance, unsupported classification, or later
normalization.

## Maintenance Rules

- New compatibility-sensitive tests must carry at least one `compat-*` label.
- A roadmap slice that changes public API, storage, file lifecycle, SQL
  behavior, or storage-engine routing should either add a harness group or
  extend an existing one.
- Do not mark a compatibility row as covered without a committed test reachable
  through the harness or a documented reason why the behavior is validated
  elsewhere.
- Keep heavyweight groups explicit. A fast unit group should not silently build
  the storage-smoke MariaDB archive.
