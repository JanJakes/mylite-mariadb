# Backup Runtime Trim

## Problem Statement

The default embedded profile still built MariaDB's external backup runtime.
That runtime implements `BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` for
server backup tooling, including global backup metadata locks and a DDL log
writer. MyLite's core profile is an embedded database-directory runtime, not a
daemon coordinated by `mariabackup`.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/sql_yacc.yy` parses `BACKUP STAGE`, `BACKUP LOCK`, and
  `BACKUP UNLOCK` into `SQLCOM_BACKUP` and `SQLCOM_BACKUP_LOCK`.
- `mariadb/sql/sql_parse.cc` dispatches those commands to
  `run_backup_stage()`, `backup_lock()`, and `backup_unlock()`.
- `mariadb/sql/backup.cc` coordinates backup stages, global backup metadata
  locks, DDL logging to `ddl.log`, and backup lock release.
- DDL paths such as `mariadb/sql/sql_insert.cc`, `mariadb/sql/sql_admin.cc`,
  and `mariadb/sql/sql_partition*.cc` call `backup_log_ddl()` as backup-tool
  hooks.

## Design

- Add `MYLITE_WITH_BACKUP_RUNTIME`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in the MyLite embedded baseline.
- When disabled, replace `backup.cc` in the embedded SQL source list with
  `mylite_backup_disabled.cc`.
- Keep ordinary DDL helper hooks inert: `backup_log_ddl()`,
  `backup_set_alter_copy_lock()`, `backup_reset_alter_copy_lock()`, and
  `backup_end()` must not fail application DDL.
- Reject `BACKUP STAGE` and `BACKUP LOCK` if MariaDB dispatch is reached
  outside the public MyLite policy. Reject `BACKUP UNLOCK` in `sql_parse.cc`
  because its helper returns `void`.
- Reject direct and prepared backup statements through the public MyLite SQL
  policy with stable unsupported-surface diagnostics.

## Compatibility Impact

External backup SQL becomes explicitly unsupported. This removes server backup
coordination commands, not application tables, DDL/DML, native storage engines,
transactions, JSON, GEOMETRY/GIS, or prepared statements.

## Directory And Lifecycle Impact

No file-format change. The trim removes the active backup runtime that could
create `ddl.log` for backup tooling. It does not add durable files, sidecars,
locks, or transient companions.

## Storage-Engine Routing Impact

No native storage-routing change. The disabled helper hooks intentionally do
nothing so ordinary DDL and storage-engine behavior remain unaffected.

## Public API Impact

No C API surface change. `mylite_exec()` and `mylite_prepare()` reject backup
SQL with stable MyLite diagnostics.

## Binary-Size Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| Foreign-server metadata trimmed | 26,553,928 bytes / 25.32 MiB | 704 | baseline |
| Backup runtime trimmed | 26,548,408 bytes / 25.32 MiB | 704 | -5,520 bytes |

The pre-strip archive moved from 27,124,416 bytes to 27,118,776 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_BACKUP_RUNTIME=OFF` appears in the embedded CMake cache.
- `backup.cc.o` is absent from `libmariadbd.a`.
- `mylite_backup_disabled.cc.o` is present in `libmariadbd.a`.
- Direct and prepared backup SQL is rejected with MyLite diagnostics.
- DDL, native storage, JSON, GEOMETRY/GIS, transactions, and prepared
  statements remain covered by the test suite.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- Backup SQL is useful for server backup tooling. A future backup/export tool
  for MyLite should work with the explicit database-directory lifecycle rather
  than exposing MariaDB server backup stages in the core library.
- Retained MariaDB DDL and transaction code still contains backup metadata-lock
  concepts. This slice only removes the active external backup runtime and
  keeps shared DDL hooks inert.
