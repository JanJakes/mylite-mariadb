# Handler Direct Exact Update

## Goal

Reduce hot prepared point-update execution overhead by letting the MyLite
handler execute the accepted single-row exact unique-key update shape through
MariaDB's direct-update hook.

The target statement shape is:

```sql
UPDATE perf_rows SET value = value + 1 WHERE id = ?
```

and equivalent predicates where a single non-nullable single-part unique key is
compared to an execution-time constant.

## Non-Goals

- Full SQL update execution inside the storage engine.
- Direct updates for scans, ranges, `OR`, `IN`, composite keys, nullable unique
  keys, prefix keys, BLOB/TEXT key parts, BLOB/TEXT row payloads,
  non-raw-exact key types, volatile `MEMORY` / `HEAP` rows, virtual/generated
  key parts, unique-key-changing updates, key-changing updates on
  foreign-key-involved tables, views, triggers, `UPDATE IGNORE`,
  row-binlogged statements, partition-changing updates, `ORDER BY`, or `LIMIT`.
- Direct `DELETE`.
- Cross-execution MariaDB prepared-plan caching.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` considers
  direct update only after quick-plan selection. It requires
  `HA_CAN_DIRECT_UPDATE_AND_DELETE`, no matching triggers, no row binlog, no
  update buffering, no `IGNORE`, no virtual columns marked read/write, and a
  single-table condition that the handler accepts through `cond_push()`.
- The same function pushes update fields and values through
  `handler::info_push(INFO_KIND_UPDATE_FIELDS/VALUES)` and calls
  `direct_update_rows_init()` before jumping to the execution block.
- The direct branch still initializes the read-record machinery before calling
  `handler::ha_direct_update_rows()`, but it skips the normal
  `info.read_record()` loop, `select->skip_record()` loop body, and
  per-row `ha_update_row()` call.
- `mariadb/sql/handler.h` documents `cond_push()` as a handler-owned condition
  stack. Returning `NULL` means the handler takes responsibility for filtering
  rows that do not match the pushed condition.
- `mariadb/sql/handler.cc::handler::ha_direct_update_rows()` wraps the virtual
  `direct_update_rows()` call with update instrumentation and transaction
  write marking.
- `mariadb/storage/spider/ha_spider.cc` is the only upstream implementation of
  the direct-update hook in this tree. It stores pushed update fields/values
  via `info_push()`, accepts pushed conditions through `cond_push()`, and
  rejects unsupported direct-update shapes in `direct_update_rows_init()`.
- `mariadb/sql/sql_base.h::fill_record()` is the SQL-layer helper used to
  evaluate update assignments into `table->record[0]`. It preserves MariaDB
  field conversion, warning, strict-mode, and simultaneous-assignment behavior.
- `mariadb/sql/sql_update.h::records_are_comparable()` and
  `compare_record()` are the helpers used by the normal update loop to suppress
  no-op updates.
- `mariadb/sql/opt_range.cc` already contains MyLite's exact unique-key update
  matcher and key serialization pattern using `store_key_item`. The direct
  handler path should use the same narrow key eligibility and MariaDB value
  conversion instead of custom key serialization.

There is no separate public MariaDB documentation for the direct-update handler
hook in the selected source tree; the MariaDB source is the implementation
authority for this slice.

## Compatibility Impact

The direct path changes only execution routing for a narrow single-table
`UPDATE` shape. MariaDB still owns parsing, name resolution, assignment
expressions, field conversion, diagnostics, autocommit statement rollback,
foreign-key hooks, CHECK/generated-column behavior, and affected-row reporting.

The handler must re-evaluate the entire pushed condition after materializing
the exact-key row. This keeps extra `AND` predicates semantically visible even
though the row candidate was found through one unique-key lookup.

`docs/COMPATIBILITY.md` does not need a support-status change because no new
SQL surface becomes supported.

## Design

Advertise `HA_CAN_DIRECT_UPDATE_AND_DELETE` from `ha_mylite::table_flags()`,
but reject every unsupported shape in the handler hook.

The handler stores direct-update execution state:

- the pushed condition accepted by `cond_push()`,
- update field/value lists received through `info_push()`,
- the matched exact unique-key number and condition value item.

`cond_push()` accepts a condition only when the handler can extract a
single-part non-null unique-key equality over the current table. The accepted
condition is not treated as fully implied by the key; `direct_update_rows()`
evaluates `pushed_cond->val_bool()` after reading the candidate row.

`direct_update_rows_init()` validates the final execution context:

- update field and value lists exist,
- a primary MyLite file is active,
- the table is a fixed-row durable MyLite-routed base table that supports row
  lifecycle operations,
- none of the fields explicitly targeted by the update are unique-key parts,
- non-unique key fields may be updated only when the table is not involved in
  parent or child foreign keys,
- the pushed condition and exact-key plan are still present,
- view/derived-table references are not involved.

`direct_update_rows()` executes:

1. Build the exact key image from the stored value item using `store_key_item`.
2. Treat NULL lookup values as no match.
3. Materialize the candidate row through
   `ha_mylite::read_exact_unique_index_row_into()` into `table->record[0]`.
4. Evaluate the full pushed condition against the materialized row.
5. Copy the old row to `record[1]`.
6. Evaluate update assignments with `fill_record(..., update=true)`.
7. Use `records_are_comparable()` / `compare_record()` to preserve no-op
   affected-row behavior.
8. Call the existing `ha_mylite::update_row(record[1], record[0])` only when
   the row changed, preserving duplicate-key, FK, autoincrement, CHECK,
   generated-column, volatile-row, rollback, and cache-maintenance behavior.

Fallback remains MariaDB's normal update loop whenever any direct-update hook
returns `HA_ERR_WRONG_COMMAND` or `cond_push()` returns the original condition.

## File Lifecycle

No file-format changes. Direct update uses the existing row update path and
therefore keeps the same primary `.mylite` file, rollback journal, volatile
row, and cache invalidation behavior as ordinary handler row updates.

## Embedded Lifecycle And API

No public `libmylite` API changes. Direct and prepared statement affected-row,
diagnostic, reset, binding, and close behavior should remain identical to the
normal update path.

## Build, Size, And Dependencies

This is a narrow MariaDB handler fork delta plus tests and docs. It adds no
dependencies. Binary-size impact should be negligible and covered by the
storage-smoke MariaDB archive build.

## Test Plan

- Add routed-storage tests for prepared primary-key updates that exercise:
  - `field = ?`,
  - `? = field`,
  - extra `AND` predicates that match,
  - extra `AND` predicates that do not match,
  - bound `NULL`,
  - no-op update affected rows,
  - key-changing duplicate failures falling back to the normal update path.
- Reuse existing routed CHECK/generated/FK update tests as regression coverage
  because the direct path calls the existing `update_row()` implementation.
- Build the storage-smoke MariaDB archive with static MyLite storage engine.
- Build storage-smoke `libmylite` tests and `mylite_perf_baseline`.
- Run focused storage-smoke tests.
- Run the prepared update component performance baseline and confirm the
  sampled direct path removes the normal quick row-read loop from the accepted
  hot statement while retaining handler update semantics inside the direct
  hook.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Accepted exact unique-key prepared updates produce the same values,
  diagnostics, and affected-row counts as the normal MariaDB update path.
- Unsupported update shapes fall back without behavior changes.
- No persistent sidecars or file-format changes are introduced.
- Focused builds, storage-smoke tests, formatting, and whitespace checks pass.
- Local performance evidence shows prepared update step time improves or stays
  neutral, and the remaining profile no longer reads the target row through the
  normal quick loop for the accepted hot statement.

## Risks And Open Questions

- Direct update bypasses the normal update loop, so the implementation must not
  skip condition evaluation, assignment evaluation, no-op comparison, or
  existing handler update checks.
- The SQL layer initializes read-record state before calling the direct hook.
  The handler path must coexist with that state and leave cleanup to the SQL
  layer.
- `fill_record()` runs inside the handler rather than `sql_update.cc`; this is
  a deliberate reuse of MariaDB semantics but increases the handler's coupling
  to SQL-layer internals.
- Wider direct-update support should wait until this exact-key path has test
  and profiling evidence.
