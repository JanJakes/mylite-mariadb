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
| `locking` | `dev` | `compat-locking` | Primary-file lock conflicts |
| `embedded-lifecycle` | `embedded-dev` | `compat-embedded-lifecycle` | Embedded runtime open/close, direct execution, and cleanup |
| `direct-sql` | `embedded-dev` | `compat-direct-sql` | `mylite_exec()` SQL execution |
| `prepared-statement` | `embedded-dev` | `compat-prepared-statement` | Prepared statements, typed bindings, and binary-safe column access |
| `storage-engine` | `storage-smoke-dev` | `compat-storage-engine` | Static handler registration and SQL storage-engine smoke |
| `sidecar` | `storage-smoke-dev` | `compat-sidecar` | Forbidden durable sidecar gates |
| `routed-ddl-dml` | `storage-smoke-dev` | `compat-routed-ddl-dml` | Routed table DDL and DML smoke |
| `application-schema` | `storage-smoke-dev` | `compat-application-schema` | WordPress-shaped application schema smoke |
| `server-surface` | `storage-smoke-dev` | `compat-server-surface` | Unsupported server-oriented surface policy |

## Relationship To MariaDB MTR

MariaDB's MTR remains the long-term source for broad upstream compatibility
cases. MyLite should not run MTR blindly as the primary local signal yet:
current MyLite behavior is embedded, file-owned, and intentionally excludes
server surfaces that many upstream suites assume. MTR integration should be a
separate comparison slice with explicit include lists, expected unsupported
surfaces, and stable result normalization.

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
