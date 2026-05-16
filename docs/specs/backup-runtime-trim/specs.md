# Backup Runtime Trim

## Goal

Remove MariaDB's external backup SQL runtime from the default MyLite embedded
profile while keeping ordinary DDL and copy `ALTER` paths working through
no-op backup bookkeeping hooks.

## Non-Goals

- Do not implement external physical backup tooling for `.mylite` files.
- Do not remove MyLite rollback-journal or future crash-recovery design work.
- Do not remove every compiled symbol whose name mentions backup. This slice
  targets MariaDB's SQL `BACKUP STAGE` / `BACKUP LOCK` runtime and DDL logging,
  not unrelated retained utility objects such as `ma_backup.c.o`.
- Do not change routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`,
  `ENGINE=BLACKHOLE`, or `ENGINE=MEMORY` DDL/DML behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/backup.cc` describes itself as the implementation of
  `BACKUP STAGE`, an interface for external backup tools.
- `mariadb/sql/backup.h` exposes `backup_init()`, `run_backup_stage()`,
  `backup_end()`, `backup_lock()`, `backup_unlock()`,
  `backup_set_alter_copy_lock()`, `backup_reset_alter_copy_lock()`, and
  `backup_log_ddl()`.
- `mariadb/sql/sql_yacc.yy` parses `BACKUP STAGE`, `BACKUP LOCK`, and
  `BACKUP UNLOCK`, using `backup_stage_names` for valid stage names.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_BACKUP` to
  `run_backup_stage()` and `SQLCOM_BACKUP_LOCK` to `backup_lock()` /
  `backup_unlock()`.
- `mariadb/sql/mysqld.cc` calls `backup_init()` during server initialization.
- `mariadb/sql/sql_class.cc` calls `backup_end()` and `backup_unlock()` during
  THD cleanup.
- Ordinary DDL paths in `handler.cc`, `sql_admin.cc`, `sql_db.cc`,
  `sql_insert.cc`, `sql_partition.cc`, `sql_partition_admin.cc`,
  `sql_table.cc`, `sql_truncate.cc`, `sql_trigger.cc`, and `sql_view.cc` call
  `backup_log_ddl()`. Copy `ALTER` paths call
  `backup_set_alter_copy_lock()` and `backup_reset_alter_copy_lock()`.
- MariaDB documentation describes `BACKUP STAGE` as SQL used by external
  backup tools:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/backup-commands/backup-stage>.
- MariaDB documentation describes `BACKUP LOCK` as DDL protection for table
  files while backup tools open and copy them:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/backup-commands/backup-lock>.

## Compatibility Impact

`BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` are server administration
surfaces for external physical backup tools. They target a datadir and
engine-owned file set, while MyLite's core runtime owns one primary `.mylite`
file and MyLite-controlled transient companions.

The public MyLite SQL API rejects representative direct and prepared backup SQL
before MariaDB execution with stable MyLite diagnostics. Ordinary DDL remains
available because the retained backup hook entry points become successful
no-ops unless a public backup command reaches the internal dispatcher.

## Design

- Add `MYLITE_WITH_BACKUP_RUNTIME`, defaulting to `ON` for upstream-like builds.
- Force `MYLITE_WITH_BACKUP_RUNTIME=OFF` from the MyLite embedded baseline.
- Replace `backup.cc` with `mylite_backup_disabled.cc` in the disabled profile
  for both the server SQL source list and the embedded SQL source list.
- Keep `backup_stage_names` available so retained parser code links.
- Keep internal cleanup and DDL hooks benign:
  - `backup_init()` is a no-op;
  - `backup_end()` resets `current_backup_stage` and succeeds;
  - `backup_unlock()` releases a leftover backup lock if one exists;
  - `backup_set_alter_copy_lock()` is a no-op;
  - `backup_reset_alter_copy_lock()` succeeds;
  - `backup_log_ddl()` is a no-op.
- Fail closed if raw MariaDB SQL dispatch reaches `run_backup_stage()` or
  `backup_lock()` outside the public MyLite SQL gate.
- Extend public SQL policy tests for direct and prepared `BACKUP STAGE`,
  `BACKUP LOCK`, and `BACKUP UNLOCK`.

## File Lifecycle

No `.mylite` file-format or companion-file change is required. The trim removes
MariaDB DDL-log writes for external backup coordination. MyLite file-level
backup, export, recovery, and checkpoint semantics remain future product
design work around the primary `.mylite` file, not inherited datadir backup SQL.

## Embedded Lifecycle And API

Startup and shutdown remain owned by `mylite_open()` / `mylite_close()`.
Retained THD cleanup can still call `backup_end()` and `backup_unlock()`
safely. Public direct/prepared SQL returns `MYLITE_ERROR` with a stable
`external backup` diagnostic for backup commands.

## Build, Size, And Dependencies

The disabled profile omits `backup.cc.o` from both the default embedded archive
and the opt-in storage-smoke archive and includes
`mylite_backup_disabled.cc.o` instead. No dependency or license change is
introduced.

Measured on 2026-05-16 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,135,920 bytes / 25.88 MiB | 671 | -7,488 bytes, unchanged |
| Storage-smoke | 27,316,504 bytes / 26.05 MiB | 674 | -7,488 bytes, unchanged |

## Test Plan

1. Reconfigure and rebuild the default MariaDB embedded archive.
2. Reconfigure and rebuild the storage-smoke archive with
   `PLUGIN_MYLITE_SE=STATIC`.
3. Confirm both archives omit `backup.cc.o` and include
   `mylite_backup_disabled.cc.o`.
4. Add direct and prepared SQL policy tests for `BACKUP STAGE START`,
   `BACKUP STAGE BLOCK_COMMIT`, `BACKUP LOCK`, and `BACKUP UNLOCK`.
5. Run affected embedded and storage-smoke CTest presets, the server-surface
   compatibility report, the size report, format, tidy, shell syntax, and diff
   checks.

## Verification Results

The implementation was verified with:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- archive scans confirming `mylite_backup_disabled.cc.o` is present and
  `backup.cc.o` is absent in both measured archives
- `cmake --preset embedded-dev`
- `cmake --preset storage-smoke-dev`
- `cmake --preset dev`
- `cmake --build --preset embedded-dev`
- `cmake --build --preset storage-smoke-dev`
- `cmake --build --preset dev`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `ctest --preset dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `tools/mylite-size-report`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness tools/mylite-size-report tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- `MYLITE_WITH_BACKUP_RUNTIME=OFF` is part of the committed embedded baseline.
- Default upstream-like builds keep `backup.cc` unless explicitly disabled.
- The default and storage-smoke archives omit `backup.cc.o` and include
  `mylite_backup_disabled.cc.o`.
- Ordinary DDL and copy `ALTER` paths still pass with no-op backup hooks.
- Public direct and prepared SQL reject backup commands before MariaDB
  execution.
- Compatibility, architecture, roadmap, and size-profile docs describe the
  unsupported boundary and measured size impact.
- Relevant tests and static checks pass.

## Risks And Open Questions

- Future `.mylite` backup/export support needs a separate file-owned design,
  likely tied to checkpoints, WAL/recovery, and write-concurrency semantics.
- Retained MDL backup-lock strategy code remains compiled because unrelated
  MariaDB locking and XA code still references backup lock types. Removing that
  deeper lock substrate would need a separate source-audited slice.
