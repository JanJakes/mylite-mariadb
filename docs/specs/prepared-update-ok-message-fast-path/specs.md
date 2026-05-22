# Prepared Update OK Message Fast Path

## Problem

After direct-update quick proof and SQL proof pushdown work, local
prepared-primary-key `UPDATE` samples still show `my_snprintf()` in
`Sql_cmd_update::update_single_table()`. MariaDB formats an OK-packet
informational string such as matched/changed/warnings for every successful
single-table `UPDATE`.

MyLite's public prepared API reads affected rows, last insert id, and warning
counts through the embedded statement and connection status fields. It does not
expose MariaDB's connection-level `mysql_info()` string, and MariaDB's prepared
statement API does not have a statement-info accessor.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` formats
  `ER_UPDATE_INFO` into a stack buffer before calling `my_ok()` on every
  successful non-`ANALYZE` single-table update.
- `mariadb/sql/sql_class.h::my_ok()` sets `THD` row count, affected rows, and
  `Diagnostics_area::set_ok_status()`. Passing a `NULL` message preserves those
  status fields and stores an empty diagnostics message.
- `mariadb/sql/sql_error.cc::Diagnostics_area::set_ok_status()` still records
  current statement warning count, affected rows, last insert id, and OK status
  when the message argument is `NULL`.
- `mariadb/libmysqld/lib_sql.cc::Protocol::net_send_ok()` copies the message
  into embedded connection data only when the message is non-`NULL`.
- `mariadb/libmysqld/lib_sql.cc::emb_stmt_execute()` copies affected rows,
  insert id, server status, and warnings back into `MYSQL_STMT` / `MYSQL`.
  There is no `mysql_stmt_info()` counterpart for the skipped message.
- `packages/libmylite/src/database.cc::mylite_step()` reads
  `mysql_stmt_affected_rows()`, `mysql_stmt_insert_id()`, and
  `mysql_warning_count()` after no-result prepared execution.

## Design

Add a small SQL-layer helper that suppresses the formatted OK message only for
the embedded prepared MyLite profile:

1. Require `EMBEDDED_LIBRARY` and `thd->current_stmt` so direct
   `mysql_query()`/`mylite_exec()` keeps MariaDB's connection-level
   `mysql_info()` behavior.
2. Require `mylite_schema_hooks_active()` so the change is limited to active
   MyLite primary-file sessions, not generic embedded MariaDB builds.
3. Keep system-versioned and period-table updates on the existing message path
   because their informational text carries an additional inserted-row count.
4. Keep bulk prepared execution on the existing message path because
   `Diagnostics_area` updates the final diagnostics message after `my_ok()`.
5. Pass `NULL` to `my_ok()` for the accepted fast path. Affected rows,
   `CLIENT_FOUND_ROWS`, last insert id, and warning count still flow through
   MariaDB's normal status machinery.

Multi-table updates remain unchanged in this slice; the current hot path is the
single-table prepared point update.

## Compatibility Impact

No public `libmylite` API behavior changes are intended. `mylite_changes()`,
`mylite_last_insert_id()`, `mylite_warning_count()`, and warning enumeration
continue to use MariaDB's status and warning APIs.

Raw direct embedded queries retain `mysql_info()` strings. Raw embedded prepared
updates in an active MyLite primary-file session may no longer populate the
connection-level `mysql_info()` string for ordinary single-table updates, but
MariaDB's prepared C API exposes affected rows and insert id through statement
accessors rather than through a statement-info string.

## Single-File And Embedded Impact

No file-format, sidecar, storage, or lifecycle changes. The guard is specific to
embedded prepared execution while MyLite schema hooks are active.

## Binary-Size Impact

The change adds one tiny helper branch in a MariaDB-derived SQL file and no
dependency.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm `my_snprintf()` no
  longer appears on the hot prepared-update OK path.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates preserve affected-row behavior for matched, changed,
  and no-match updates.
- Prepared update warning capture remains correct.
- Direct `mylite_exec()` / embedded text-query updates keep formatted
  `mysql_info()` behavior.
- Hot prepared-update samples avoid the per-execute OK-message formatting frame.

## Risks

- Over-broad suppression would change direct embedded `mysql_info()` behavior;
  the helper must stay limited to prepared execution.
- Bulk or system-versioned updates rely on specialized message content and must
  remain on the existing formatting path.
- The optimization does not remove any storage work; if `my_snprintf()` was only
  a sampling artifact, measured timing may stay within local noise.
