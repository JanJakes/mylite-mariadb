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
| `crash-recovery` | `dev` | `compat-crash-recovery` | Rollback-journal recovery and corrupt-journal handling |
| `transaction` | `storage-smoke-dev` | `compat-transaction` | SQL transaction-control rejection until full transaction support exists |
| `transaction-hooks` | `storage-smoke-dev` | `compat-transaction-hooks` | MariaDB statement transaction hook integration |
| `statement-rollback` | `storage-smoke-dev` | `compat-statement-rollback` | Failed statement rollback for routed MyLite storage, including representative OR REPLACE replacement and table-DDL failures |
| `partition` | `storage-smoke-dev` | `compat-partition` | Partition DDL rejection until partition metadata and routing exist |
| `foreign-key` | `storage-smoke-dev` | `compat-foreign-key` | Foreign-key DDL rejection until catalog metadata and enforcement exist |
| `check-constraint` | `storage-smoke-dev` | `compat-check-constraint` | CHECK constraint enforcement, ALTER existence-option skips, CTAS targets, and failed ADD CHECK rollback on routed tables |
| `generated-column` | `storage-smoke-dev` | `compat-generated-column` | Generated column read/write, CTAS projection, ALTER, BLOB/TEXT prefix, and generated-column index coverage |
| `unsupported-index` | `storage-smoke-dev` | `compat-unsupported-index` | Unsupported FULLTEXT, SPATIAL, and long-unique index rejection |
| `locking` | `storage-smoke-dev` | `compat-locking` | Primary-file lock conflicts, busy timeouts, and SQL locking policy |
| `embedded-lifecycle` | `embedded-dev` | `compat-embedded-lifecycle` | Embedded runtime open/close, direct execution, and cleanup |
| `direct-sql` | `embedded-dev` | `compat-direct-sql` | `mylite_exec()` SQL execution and statement effects |
| `prepared-statement` | `embedded-dev` | `compat-prepared-statement` | Prepared statements, typed bindings, binary-safe column access, and statement effects |
| `column-metadata` | `embedded-dev` | `compat-column-metadata` | Prepared statement column metadata |
| `large-value` | `embedded-dev` | `compat-large-value` | Prepared statement large-value segment reads |
| `warning` | `embedded-dev` | `compat-warning` | Warning counts and structured `SHOW WARNINGS` rows |
| `sql-comparison` | `embedded-dev` | `compat-sql-comparison` | MariaDB baseline SQL API comparison |
| `storage-engine` | `storage-smoke-dev` | `compat-storage-engine` | Static handler registration and SQL storage-engine smoke |
| `sidecar` | `storage-smoke-dev` | `compat-sidecar` | Forbidden durable sidecar gates |
| `routed-ddl-dml` | `storage-smoke-dev` | `compat-routed-ddl-dml` | Routed direct/prepared schema namespaces, directory-free file-backed `CREATE DATABASE`, catalog-backed `CREATE DATABASE` existence options, table and index DDL, ordinary `CREATE TABLE IF NOT EXISTS`, column ALTER existence-option skips, index DDL existence-option skips, index rename existence-option skips, index ignorability, non-CHECK primary/unique constraint DDL including primary-key add/drop/re-add and failed ADD UNIQUE rollback, CHECK constraint existence-option skips, indexed table/index rename, table-DDL `IF EXISTS` skips, failed table-DDL rollback, temporary LIKE/CTAS isolation, shadowing, and OR REPLACE, online/in-place ALTER rejection, `CREATE TABLE ... SELECT` duplicate modes, plain/LIKE/CTAS OR REPLACE, truncate, and DML smoke |
| `application-schema` | `storage-smoke-dev` | `compat-application-schema` | WordPress-shaped core schema, WordPress installer DDL and seed fixtures, collation restart matrix, and prefix-index smoke |
| `server-surface` | `storage-smoke-dev` | `compat-server-surface` | Unsupported server and non-table object surface policy |

## Relationship To MariaDB MTR

The `sql-comparison` group compares representative direct execution, prepared
statement, metadata, and warning behavior against a raw MariaDB embedded
runtime. MariaDB's MTR remains the long-term source for broad upstream
compatibility cases. MyLite should not run MTR blindly as the primary local
signal yet: current MyLite behavior is embedded, file-owned, and intentionally
excludes server surfaces that many upstream suites assume. The opt-in
`tools/mylite-mtr-harness` runner proves the embedded MTR path with a tiny
curated upstream smoke test, but it remains outside the default compatibility
groups because it builds `mariadbd` and several upstream client/support tools.
Broader MTR integration should be a separate comparison slice with explicit
include lists, expected unsupported surfaces, and stable result normalization.

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
