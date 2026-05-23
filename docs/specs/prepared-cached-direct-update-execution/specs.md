# Prepared Cached Direct Update Execution

## Problem

Prepared MyLite point updates now reach the handler direct-update path, but
`Sql_cmd_update::update_single_table()` still repeats a large part of MariaDB's
single-table update planning before it calls `ha_direct_update_rows()`.
The local `prepared-row-only-update-miss-components` phase is useful here
because it measures bind, step, and reset for an exact-key prepared update that
finds no row and does not perform storage mutation. A current local sample over
10000 rows and 1000000 iterations measures the step component at about
1.16 us/op, while prepared primary-key point selects are about 0.28 us/op for
the row component.

Full prepared DML reuse is still blocked by `unit->cleanup()` and DML
unprepare semantics. This slice targets a narrower execution-stage shortcut:
when a previous execution already proved a MyLite single-table exact-key direct
update shape, use that cached proof in `update_single_table()` before the
normal quick-select planning path. Unsupported or stale shapes must fall back
to MariaDB's existing update planning.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_select.cc::Sql_cmd_dml::execute()` still prepares DML when
  `!is_prepared()`, executes, calls `unit->cleanup()`, and then calls
  `unprepare(thd)`. Its source comment says the already-prepared DML branch is
  currently never used because cleanup effectively unprepares DML commands.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` creates the
  `JOIN`, runs `setup_tables()`, calls `JOIN::prepare()`, and prepares update
  values for the single-update result-elision path.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` handles
  single-table update execution after prepare. Before direct update it still
  runs table statistics, `make_select()`, `SQL_SELECT::check_quick()`,
  subquery optimization cleanup, direct-update eligibility checks, and proof
  pushdown to the handler.
- `mariadb/sql/opt_range.cc::SQL_SELECT::store_mylite_update_exact_key_proof_cache()`
  stores the exact-key update proof on `Sql_cmd_update` after range analysis
  recognizes a simple unique-key equality that the MyLite handler can use.
- `mariadb/sql/opt_range.cc::try_simple_update_unique_key_quick_select()` can
  reuse that proof on later executions, but only after `make_select()` and
  `SQL_SELECT::check_quick()` are reached.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::info_push()` accepts
  `INFO_KIND_MYLITE_UPDATE_EXACT_KEY`, `INFO_KIND_UPDATE_FIELDS`, and
  `INFO_KIND_UPDATE_VALUES` to configure direct update. The handler validates
  the key number and supported key shape before accepting the proof.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` builds
  the runtime key, reads the exact unique row into `table->record[0]`, evaluates
  a residual condition when needed, fills `table->record[0]`, enforces CHECK
  constraints, prepares high-level indexes, and calls `update_row()`.

## Design

Add a MyLite-only fast path near the start of
`Sql_cmd_update::update_single_table()` after the update table is known and
basic table updatability has been checked. The fast path may execute only when
all of these are true:

- MyLite schema hooks are active and the statement is an embedded prepared
  statement.
- The statement is a single-table update against a base table, not a view,
  derived table, period table, multitable update, `RETURNING`, `EXPLAIN`,
  `ANALYZE`, `ORDER BY`, `LIMIT`, or `UPDATE IGNORE`.
- The select tree and update values have no subqueries.
- The table has no matching update triggers, row binlogging is not active, and
  virtual columns are not marked for read or write.
- `Mylite_update_exact_key_proof_cache` is valid for the current prepared
  condition pointer and names a key still present in the opened table.
- The MyLite handler accepts the cached proof and the current update field and
  value lists through the same `info_push()` / `direct_update_rows_init()`
  contract used by the normal direct-update path.

When these conditions hold, execute `ha_direct_update_rows()` directly and feed
the resulting `found` and `updated` counts into the existing update completion
logic. On any eligibility miss or handler rejection before execution, clear any
temporary pushed handler condition and fall back to the normal
`update_single_table()` path. Once execution begins, handler errors must be
reported through the existing update error handling path rather than silently
falling back.

This is deliberately not full prepared DML reuse. It does not skip
`Sql_cmd_dml::prepare()`, `Sql_cmd_update::prepare_inner()`, `JOIN::prepare()`,
table opening, table locking, or value setup. Those remain separate
`prepared-dml-execution-reuse` work.

## Affected Subsystems

- MariaDB single-table update execution in `sql_update.cc`.
- MyLite exact-key direct-update proof reuse in `opt_range.cc`.
- MyLite handler direct-update proof pushdown in `storage/mylite/ha_mylite.cc`.
- Local prepared-update performance baseline phases.

## Compatibility Impact

The expected SQL-visible behavior is unchanged. The fast path is restricted to
the same handler direct-update shape that the normal MariaDB update path already
accepts. MariaDB remains authoritative for statement preparation, field
resolution, expression setup, metadata revalidation, diagnostics, strict-mode
conversion, affected rows, generated columns, CHECK constraints, foreign keys,
rollback, and warnings.

Unsupported or stale shapes must continue through the existing path. This is
especially important for triggers, views, period tables, versioned behavior,
subqueries, `RETURNING`, `ORDER BY`, `LIMIT`, `IGNORE`, binlogging, full text,
virtual columns, and multitable update conversion.

## Single-File And Embedded Lifecycle Impact

No file-format or public API change is required. The shortcut uses the existing
MyLite handler and storage update path, so durable state remains in the primary
`.mylite` file and any transient companions remain under the existing storage
lifecycle.

## Binary-Size And Dependency Impact

No new dependency. The fork delta should be a narrow MyLite-owned helper in
`sql_update.cc`; measure the embedded archive size after implementation.

## Tests And Verification

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build `mylite_perf_baseline`, `mylite_embedded_statement_test`, and
  `mylite_embedded_storage_engine_test`.
- Run focused embedded statement and storage-engine tests.
- Run the full storage-smoke preset if the focused checks pass.
- Run `prepared-row-only-update-miss-components`,
  `prepared-row-only-update-components`, and
  `prepared-assignment-update-components` to compare step timings.
- Capture or review a focused sample to confirm time moved out of
  `Sql_cmd_update::update_single_table()` planning before claiming a speedup.
- Run `git diff --check` and formatting checks for touched MariaDB files.

Exploratory implementation result:

- A narrow helper in `Sql_cmd_update::update_single_table()` was implemented
  locally and was confirmed reachable in a focused sample; the sample showed
  `mylite_execute_cached_direct_update()` calling `ha_direct_update_rows()`
  directly.
- Focused embedded statement and storage-engine tests passed after the helper
  was linked.
- The local storage-smoke archive grew from `21197904` bytes to `21198928`
  bytes.
- The component timings did not improve enough to keep the fork delta:
  `prepared-row-only-update-miss-components` stayed around `1.166 us/op`,
  `prepared-row-only-update-components` measured around `1.684 us/op`, and
  `prepared-assignment-update-components` measured around `1.681 us/op`.
- The attempted helper was reverted. The result suggests this exact shortcut is
  too late in `update_single_table()` to matter by itself; the useful next
  boundary remains the broader `Sql_cmd_dml::prepare()` /
  `Sql_cmd_update::prepare_inner()` reuse split tracked by
  [Prepared DML execution reuse](../prepared-dml-execution-reuse/specs.md).

## Acceptance Criteria

- Eligible prepared MyLite exact-key updates still return correct affected-row
  and found-row behavior for match, no-match, and unchanged-row cases.
- Unsupported update shapes fall back to the existing MariaDB update planning
  path.
- Handler errors after execution starts are reported normally.
- Focused tests pass and the local component benchmark shows a meaningful
  `prepared-row-only-update-miss` or row-only update step reduction before the
  change is kept.

## Risks And Unresolved Questions

- The cached proof is only a shape proof. It must not be used to retain open
  tables, `JOIN` state, row buffers, handler state, diagnostics, or locks
  across executions.
- If a later MariaDB cleanup path invalidates the condition or value item
  pointer differently, the pointer match and handler validation must force
  fallback.
- The completion path in `update_single_table()` owns binlog, query-cache,
  warning, transaction, and OK-packet side effects. The shortcut must rejoin
  that existing path rather than adding a parallel completion path.
