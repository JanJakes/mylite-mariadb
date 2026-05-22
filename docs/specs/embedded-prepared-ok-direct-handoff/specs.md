# Embedded Prepared OK Direct Handoff

## Problem

After the prepared-update OK message fast path, local prepared-primary-key
`UPDATE` samples still show `Protocol::net_send_ok()` allocating a
`MYSQL_DATA` / `embedded_query_result` pair for each successful no-result
prepared execution. The allocation is immediately consumed by
`emb_stmt_execute()` only to copy affected rows, insert id, warning count, and
server status into the embedded connection and statement fields.

For MyLite's public prepared API, those scalar status fields are the observable
result. The per-execute embedded result object is useful for generic embedded
MariaDB response delivery, but it is unnecessary for guarded MyLite prepared OK
responses that have no result-set metadata, rows, error payload, OK info
string, or pending multi-result response.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/libmysqld/lib_sql.cc::emb_advanced_command()` resets connection
  result fields, clears the embedded data list, sets `thd->current_stmt` when
  the client layer supplies a statement handle, and then dispatches the command.
  That statement pointer is broader than execute-only, so the fast path needs
  a separate execute-only marker.
- `mariadb/libmysqld/lib_sql.cc::Protocol::net_send_ok()` allocates a new
  embedded result object with `THD::alloc_new_dataset()`, stores affected rows,
  insert id, and optional info text, and then delegates warning and status
  storage to `write_eof_packet()`.
- `mariadb/libmysqld/lib_sql.cc::write_eof_packet()` stores server status and
  a capped warning count on `thd->cur_data->embedded_info`, with stored
  procedure warning-count suppression.
- `mariadb/libmysqld/lib_sql.cc::emb_read_query_result()` consumes
  `thd->first_data`, copies warning count, server status, field count,
  affected rows, insert id, and optional info text to `MYSQL`, and frees the OK
  result object when no fields are present.
- `mariadb/libmysqld/lib_sql.cc::emb_stmt_execute()` always calls
  `emb_read_query_result()` after successful `COM_STMT_EXECUTE`, then copies
  `MYSQL` affected rows, insert id, and server status to `MYSQL_STMT`.
- `mariadb/include/mysql.h::MYSQL` stores the scalar OK fields that MyLite
  reads after prepared execution: affected rows, insert id, server status,
  warning count, field count, fields, and info pointer.
- `mariadb/include/mysql.h::MYSQL_STMT` stores copies of affected rows, insert
  id, and server status. The prepared-statement API has no `mysql_stmt_info()`
  accessor.

## Design

Add a MyLite-owned embedded fast path that copies scalar OK state directly to
`MYSQL` for eligible prepared no-result executions:

1. Set an embedded-only thread-local marker around `COM_STMT_EXECUTE` dispatch
   in `emb_advanced_command()`, preserving any previous marker value across
   nested calls.
2. In `Protocol::net_send_ok()`, accept the direct handoff only when:
   - `thd->current_stmt` is set and the execute-only marker is active, meaning
     embedded prepared execution is active rather than prepare/reset/close
     handling;
   - `mylite_schema_hooks_active()` is true, limiting the change to active
     MyLite primary-file sessions;
   - the OK message is `NULL` or empty, preserving direct-query `mysql_info()`
     and any statement that deliberately reports non-empty info text;
   - `SERVER_MORE_RESULTS_EXISTS` is absent, keeping multi-result and stored
     routine surfaces on the existing embedded result queue.
3. For accepted responses, copy warning count, server status, affected rows,
   insert id, zero field metadata, and clear the connection info pointer
   directly into `MYSQL`. Preserve `write_eof_packet()`'s stored-procedure
   warning-count suppression and fatal-error server-status side effect.
4. Do not add new `THD` layout state for this optimization. In
   `emb_stmt_execute()`, treat the response as directly handed off only when no
   embedded result object was queued and `MYSQL::affected_rows` no longer holds
   the command-start sentinel. The existing statement copies from `MYSQL` to
   `MYSQL_STMT` then preserve `mysql_stmt_affected_rows()`,
   `mysql_stmt_insert_id()`, and statement server-status behavior.

The fast path is deliberately embedded-only and generic across MyLite prepared
OK responses with no info text. It is not update-specific; the update message
slice makes ordinary prepared updates eligible by passing `NULL` to `my_ok()`.

## Affected MariaDB Subsystems

- Embedded libmysqld result delivery in `mariadb/libmysqld/lib_sql.cc`.
- Embedded-only prepared execute tracking in `mariadb/libmysqld/lib_sql.cc`.

No SQL parser, optimizer, handler, storage, catalog, or file-format behavior
changes.

## Compatibility Impact

No public `libmylite` API behavior changes are intended. `mylite_changes()`,
`mylite_last_insert_id()`, `mylite_warning_count()`, and prepared statement
success/failure behavior continue to use MariaDB's scalar status fields.

Direct embedded text queries keep MariaDB's normal `MYSQL_DATA` response path
and `mysql_info()` behavior because they do not set `thd->current_stmt`.
Prepared statements that produce fields, errors, warnings with result payloads,
OK info text, or multi-result responses keep the normal embedded result queue.

Raw embedded prepared MyLite statements with a `NULL` or empty OK info string
receive the same scalar statement status without an intermediate embedded OK
result object.

## DDL Metadata Routing Impact

No catalog, table-definition, or DDL metadata routing behavior changes. Prepared
DDL with no OK info string may use the direct scalar handoff only after the DDL
has already completed through existing MariaDB and MyLite catalog paths.

## Single-File And Embedded Lifecycle Impact

No file-format, durable state, sidecar, recovery, or lock lifecycle changes.
The change affects only in-process embedded response handoff for active MyLite
sessions.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No storage-engine routing change. Eligibility is keyed to active MyLite schema
hooks only to avoid changing generic embedded MariaDB builds.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes. This code is compiled only for the embedded
library path and does not affect daemon/network packet serialization.

## Binary-Size Impact

The slice adds one embedded-only boolean marker and a small helper branch. It
adds no dependency. Archive-size impact should be neutral to negligible and
measured through the storage-smoke embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the hot path no longer
  reports per-execute `THD::alloc_new_dataset()` / `my_malloc()` under
  `Protocol::net_send_ok()`.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates preserve affected-row behavior for changed, unchanged,
  and no-match updates.
- Prepared update warning count remains visible through `mylite_warning_count()`.
- Direct text-query updates keep formatted `mysql_info()` behavior.
- Hot prepared-update samples avoid the embedded OK result allocation frame.

## Risks And Unresolved Questions

- Over-broad eligibility could bypass the embedded result queue for result-set,
  error, or multi-result responses. The guard must stay limited to prepared OK
  responses with no info string and no pending additional results.
- Generic embedded MariaDB users should not see behavior changes; the guard must
  require active MyLite hooks and the execute-only prepared marker.
- The direct handoff removes response allocation overhead, but storage mutation
  and handler work remain the dominant long-term performance targets.
