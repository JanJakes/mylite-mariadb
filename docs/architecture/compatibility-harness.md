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
| `storage-core` | `dev` | `compat-storage` | Primary file, catalog, rows, indexes, and mutations |
| `crash-recovery` | `dev` | `compat-crash-recovery` | Rollback-journal, transaction-journal, and corrupt-journal recovery |
| `transaction` | `storage-smoke-dev` | `compat-transaction` | Direct and prepared row-DML transaction start/completion, supported direct and prepared session autocommit mode, read-only/read-write, isolation-level, transaction variable, temporary row-DML, and completion-type transaction SET controls, direct and prepared case-insensitive quoted savepoint rollback/release, active transaction crash recovery, and unsupported transaction-control policy |
| `transaction-hooks` | `storage-smoke-dev` | `compat-transaction-hooks` | MariaDB statement transaction hook integration |
| `statement-rollback` | `storage-smoke-dev` | `compat-statement-rollback` | Failed statement rollback for routed MyLite storage, including prepared row-DML, representative row `REPLACE`, OR REPLACE replacement, and table-DDL failures |
| `partition` | `storage-smoke-dev` | `compat-partition` | Partition DDL rejection until partition metadata and routing exist |
| `foreign-key` | `storage-smoke-dev` | `compat-foreign-key` | Foreign-key DDL publication, metadata, row checks, bounded actions, and unsupported-action policy coverage |
| `check-constraint` | `storage-smoke-dev` | `compat-check-constraint` | CHECK constraint enforcement, ALTER existence-option skips, CTAS targets, and failed ADD CHECK rollback on routed tables |
| `generated-column` | `storage-smoke-dev` | `compat-generated-column` | Generated column read/write, CTAS projection, ALTER, BLOB/TEXT prefix, and generated-column index coverage |
| `unsupported-index` | `storage-smoke-dev` | `compat-unsupported-index` | Unsupported FULLTEXT, SPATIAL, vector, and long-unique index rejection |
| `locking` | `storage-smoke-dev` | `compat-locking` | Primary-file lock conflicts, busy timeouts, and SQL locking policy |
| `embedded-lifecycle` | `embedded-dev` | `compat-embedded-lifecycle` | Embedded runtime open/close, direct execution, and cleanup |
| `direct-sql` | `embedded-dev` | `compat-direct-sql` | `mylite_exec()` SQL execution and statement effects |
| `prepared-statement` | `embedded-dev` | `compat-prepared-statement` | Prepared statements, typed bindings, binary-safe column access, and statement effects |
| `column-metadata` | `embedded-dev` | `compat-column-metadata` | Prepared statement column metadata |
| `large-value` | `embedded-dev` | `compat-large-value` | Prepared statement large-value segment reads |
| `warning` | `embedded-dev` | `compat-warning` | Warning counts and structured `SHOW WARNINGS` rows |
| `sql-comparison` | `embedded-dev` | `compat-sql-comparison` | MariaDB baseline SQL API comparison |
| `storage-engine` | `storage-smoke-dev` | `compat-storage-engine` | Static handler registration, routed engine metadata, BLACKHOLE row-discard smoke, and MEMORY/HEAP volatile-row smoke |
| `sidecar` | `storage-smoke-dev` | `compat-sidecar` | Forbidden durable sidecar gates |
| `routed-ddl-dml` | `storage-smoke-dev` | `compat-routed-ddl-dml` | Routed direct/prepared schema namespaces, directory-free file-backed `CREATE DATABASE`, catalog-backed `CREATE DATABASE` existence options, table and index DDL, ordinary `CREATE TABLE IF NOT EXISTS`, column ALTER existence-option skips, index DDL existence-option skips, index rename existence-option skips, index ignorability, non-CHECK primary/unique constraint DDL including primary-key add/drop/re-add and failed ADD UNIQUE rollback, CHECK constraint existence-option skips, indexed table/index rename, table-DDL `IF EXISTS` skips, failed table-DDL rollback, temporary LIKE/CTAS isolation, shadowing, and OR REPLACE, online/in-place ALTER rejection, `CREATE TABLE ... SELECT` duplicate modes, plain/LIKE/CTAS OR REPLACE, truncate, and DML smoke |
| `application-schema` | `storage-smoke-dev` | `compat-application-schema` | WordPress-shaped core schema, WordPress installer DDL and seed fixtures including deterministic default options and role payloads, WordPress multisite schema/seed fixtures, BuddyPress component schema/seed fixtures, Laravel, Django, and Rails Active Record schema fixtures, expanded collation restart matrix, and prefix-index smoke |
| `server-surface` | `storage-smoke-dev` | `compat-server-surface` | Unsupported server, binlog/replication, replication/binlog filter assignment, binlog/replication system-variable assignment and omitted-variable introspection, dynamic plugin loading, SQL HANDLER, SQL sequence values, virtual sequence storage engine, SQL HELP, SELECT PROCEDURE, SQL file-I/O, table-maintenance/key-cache administration, native CSV/InnoDB/MyISAM/MRG/partition engine absence, unsupported engine request rejection including known external no-equals names, user-statistics, statement profiling, optimizer trace, static SHOW information, status metadata, process-list metadata, view metadata, routine metadata, trigger metadata, foreign-server metadata, external backup SQL, query cache administration, zlib compression, server utility function, Oracle SQL mode, XML SQL function, GIS SQL function, vector SQL function, SFORMAT SQL function, JSON schema validation function, JSON table function, dynamic column function, view runtime, stored-program runtime, trigger runtime, dynamic UDF runtime, and non-table object surface policy |

## Relationship To MariaDB MTR

The `sql-comparison` group compares representative direct execution, prepared
statement, metadata, and warning behavior against a raw MariaDB embedded
runtime. MariaDB's MTR remains the long-term source for broad upstream
compatibility cases. MyLite should not run MTR blindly as the primary local
signal yet: current MyLite behavior is embedded, file-owned, and intentionally
excludes server surfaces that many upstream suites assume. The opt-in
`tools/mylite-mtr-harness` runner proves the embedded MTR path with a curated
smoke list covering the MyLite trimmed bootstrap schema, upstream scalar
CAST/CONVERT, CASE-family expression, selected numeric/date, parser/comment,
comparison, `ORDER BY`, scalar operator, string/format, crypto/KDF,
disabled DES, aggregate DISTINCT, date-format, and ASCII charset behavior. It
uses a separate
`build/mariadb-mtr-smoke`
profile because the default embedded profile intentionally omits view, stored
program, trigger, and binlog sysvar surfaces that MTR bootstrap still expects.
It remains outside the default compatibility groups because it builds
`mariadbd` and several upstream client/support tools. Broader MTR integration
should be a separate comparison slice with explicit include lists, expected
unsupported surfaces, and stable result normalization.

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
