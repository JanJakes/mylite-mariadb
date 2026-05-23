# Prepared Direct Update Rebind

## Problem

The current prepared row-only update hot path has reached diminishing returns in
the MyLite storage layer. A local `prepared-row-only-update-components` sample
over 10000 rows and 5000000 iterations measured:

- bind: about `0.022 us/op`
- step: about `1.60 us/op`
- reset: about `0.022 us/op`

The storage row mutation component is about `0.33 us/op` locally, so the next
material gap is in MariaDB's prepared `UPDATE` execution path: table opening,
locking, DML preparation, `JOIN::prepare()`, and update value setup. MyLite
already caches exact-key proof and handler direct-update shape state, but those
caches are reached only after MariaDB has rebuilt enough statement state for the
current execution.

The next slice must define a safe table-rebind boundary before attempting to
reuse more prepared update shape. Previous shortcuts that retained only part of
the update setup were rejected because `unit->cleanup()` and DML unprepare leave
stale expression and table state.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc::check_prepared_statement()` validates
  `UPDATE`, `DELETE`, and multi-table DML by calling
  `lex->m_sql_cmd->prepare(thd)` and immediately
  `lex->m_sql_cmd->unprepare(thd)` on success. `PREPARE` therefore validates
  shape but does not leave DML command state prepared for `EXECUTE`.
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute()` calls
  `reinit_stmt_before_use(thd, lex)` immediately before
  `mysql_execute_command(thd, true)`.
- `mariadb/sql/sql_prepare.cc::reinit_stmt_before_use()` rebuilds copied WHERE
  and HAVING expression trees and asserts `SELECT_LEX::join == 0`.
- `mariadb/sql/table.cc::TABLE_LIST::reinit_before_use()` clears
  `TABLE_LIST::table` and the MDL ticket because the old table object was
  closed after the previous prepare or execute call.
- `mariadb/sql/sql_parse.cc::mysql_execute_command()` calls
  `close_thread_tables_for_query(thd)` after statement transaction
  commit/rollback. A prepared-DML reuse path cannot keep old `TABLE *`
  references alive by changing only `Sql_cmd_update`.
- `mariadb/sql/sql_select.cc::Sql_cmd_dml::prepare()` performs precheck,
  `open_tables_for_query()`, and command-specific `prepare_inner()` before
  setting the DML command prepared.
- `mariadb/sql/sql_select.cc::Sql_cmd_dml::execute()` currently prepares DML
  when `!is_prepared()`, locks tables, executes, calls `unit->cleanup()`, and
  then calls `unprepare(thd)`. Its source comment says the already-prepared DML
  branch is currently never used because cleanup effectively unprepares DML
  commands.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` creates a new
  `JOIN`, calls `setup_tables()`, `vers_setup_conds()`, sets
  `SELECT_LEX::item_list_usage = MARK_COLUMNS_WRITE`, and calls
  `JOIN::prepare()`. For MyLite's single-update result-elision path it then
  runs `mylite_prepare_single_update_values()` and associates update values
  with target fields.
- `mariadb/sql/sql_select.cc::JOIN::prepare()` stores the prepared WHERE
  condition in `SELECT_LEX::where_cond_after_prepare`. Skipping
  `JOIN::prepare()` without a replacement rebind step would leave this pointer
  stale after `reinit_stmt_before_use()`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` still
  calls `make_select()`, `SQL_SELECT::check_quick()`, update-column marking,
  subquery optimization cleanup, direct-update eligibility checks, and handler
  proof pushdown before `ha_direct_update_rows()`.
- `mariadb/sql/opt_range.h::Mylite_update_exact_key_proof_cache` stores only a
  condition pointer, key number, value item, and whether the key implies the
  full condition. It is a proof cache, not a rebind cache.
- `mariadb/sql/opt_range.cc::try_simple_update_unique_key_quick_select()` can
  reuse that proof when the cached condition pointer still matches the current
  `SQL_SELECT::cond`, but this reuse currently happens after SQL-layer prepare
  has rebuilt the execution state.
- `mariadb/sql/handler.h::mylite_update_exact_key_info` carries the exact-key
  proof to the handler, and `handler::ha_direct_update_rows()` is the existing
  MariaDB hook MyLite uses for accepted direct updates.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` still
  evaluates the pushed condition, fills the target row with MariaDB update
  values, enforces row checks through the normal handler update path, and
  reports found/updated rows through MariaDB's direct-update contract.

## Design

Add a MyLite-owned prepared direct-update shape that is independent of retained
`JOIN`, `TABLE`, handler, lock, diagnostics, and row-buffer state. The shape is
allowed to cache only immutable facts that can be validated against the freshly
opened table on every execution:

- single-table `UPDATE`,
- MyLite-routed file-backed base table,
- no view, derived table, period table, versioned-table behavior, trigger path,
  `RETURNING`, `ORDER BY`, `LIMIT`, `IGNORE`, `EXPLAIN`, `ANALYZE`, row-binlog
  path, or subquery-dependent shape,
- exact non-null single-part unique-key predicate accepted by MyLite's
  SQL-layer proof cache,
- target write set and whether updated fields can change direct-unsafe keys,
- whether update values need normal `setup_fields()`.

The implementation boundary should be:

1. Leave the first execution on the current MariaDB path. It validates the
   statement, opens and locks tables, prepares the `JOIN`, and populates the
   existing MyLite proof and handler-shape caches.
2. On later prepared executions, only attempt the shortcut when the cached
   shape says the statement was previously accepted and no global SQL mode,
   slow-log explain, metadata epoch, temporary-table lifecycle, or transaction
   setting invalidates that shape.
3. Run precheck and `open_tables_for_query()` normally. Do not retain open
   tables or MDL tickets in this slice.
4. Rebind the cached shape against the freshly opened `TABLE_LIST::table`.
   This step must replace stale field/table pointers, refresh
   `where_cond_after_prepare`, mark the update write set, run assignability and
   value setup when required, and revalidate that the named key and field shape
   still match the cached proof.
5. Only after the rebind succeeds, lock tables and enter a direct-update
   execution helper that pushes the current exact-key proof, update fields, and
   update values to the handler, calls `ha_direct_update_rows()`, and rejoins
   the existing `update_single_table()` completion semantics for affected rows,
   warnings, binlog/query-cache hooks, statement status, and handler cleanup.
6. If any eligibility or rebind check fails before handler execution starts,
   discard the shortcut state for this execution and run the existing MariaDB
   prepare path. If handler execution starts, report errors through the normal
   update error path rather than falling back.

This is not table-handle reuse. It keeps MariaDB's per-execution table open and
lock lifetime intact while carving out only the repeated `JOIN::prepare()` and
range-planning work for the already-proven MyLite direct-update subset.

The first implementation step is behavior-neutral: when the normal execution
path accepts a MyLite exact-key direct update for an embedded prepared
statement, `Sql_cmd_update` records the accepted key number, value item,
condition coverage, and simple value-setup classification in explicit
prepared-direct-update shape fields. The cache is cleared whenever the current
execution does not take that accepted MyLite proof path. Later rebind work must
validate these cached facts against the freshly opened table before using them.

The second implementation step adds that validation boundary without enabling
the direct execution shortcut. Accepted normal executions now also record the
accepted key field index and field name. On later executions, when MariaDB's
existing proof-cache condition pointer no longer matches the current prepared
condition tree, `Sql_cmd_update` validates the cached key number, key field
fingerprint, MariaDB table reference type/version, simple unique-key shape,
value-list setup classification, and key value item against the freshly opened
table and current condition tree before retargeting the exact-key proof cache
to the current condition. Simple prepared updates whose existing
condition-pointer proof and table reference are still current skip this extra
validation so the hottest row-only update loop does not pay for future
shortcut scaffolding. Clearing the prepared shape also clears the SQL-layer
exact-key proof cache, so unsupported or changed shapes fail closed into the
normal MariaDB update path. Repeated executions that accept the same proof
against the same MariaDB table reference keep the existing prepared shape
instead of re-walking and re-copying the key-field fingerprint.

## Non-Goals

- Retain `TABLE *`, MDL tickets, handler objects, `JOIN` objects, row buffers,
  diagnostics, warnings, locks, or transaction state across executions.
- Skip precheck, table opening, table locking, reprepare invalidation, or
  metadata-shape validation.
- Support scans, ranges, composite keys, nullable unique keys, generated-key
  predicates, key-changing updates, BLOB/TEXT direct-update fast paths,
  triggers, views, partition behavior, period/versioned behavior,
  `ORDER BY`, `LIMIT`, `IGNORE`, `RETURNING`, subqueries, or multi-table
  updates.
- Change public `libmylite` APIs or the `.mylite` file format.
- Add a SQL-string parser in `libmylite` to bypass MariaDB semantics.

## Affected Subsystems

- MariaDB prepared statement execution in `sql_prepare.cc`.
- DML prepare/execute cleanup in `sql_select.cc`.
- Single-table `UPDATE` prepare and execution in `sql_update.cc`.
- MyLite exact-key update proof caching in `opt_range.cc`.
- MyLite handler direct-update proof pushdown in `storage/mylite/ha_mylite.cc`.
- `libmylite` prepared statement performance.

## Compatibility Impact

No SQL-visible behavior change is intended. MariaDB remains authoritative for
metadata validation, name resolution, field assignment, expression evaluation,
generated columns, CHECK constraints, foreign keys, strict conversion,
warnings, affected-row counts, unchanged-row behavior, diagnostics, and
transaction rollback.

Unsupported shapes must continue through the existing MariaDB update path.
Metadata changes, temporary table shadowing, reprepare requests, changed SQL
mode, explain-observable modes, and any changed table definition must invalidate
or bypass the cached direct-update shape.

## Single-File And Embedded Lifecycle Impact

No durable file-format change. The direct-update execution still uses the
existing MyLite handler and storage update path, so durable application state
remains in the primary `.mylite` file and transient journal/lock companions
keep their existing lifecycle.

The cache is statement-owned, process-local state. It must be discarded when a
prepared statement is finalized, reprepared, or when MyLite metadata lifecycle
tracking says the relevant storage metadata may have changed.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. The expected binary-size impact is a small MariaDB fork
delta in `sql_update.cc` / `sql_update.h` plus narrow helper code. Measure the
storage-smoke `libmariadbd.a` size before keeping an implementation.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Add focused routed-storage tests for:
  - first execution using the normal path and later executions using the
    shortcut,
  - matched row, no-match row, and unchanged-row affected counts,
  - bound `NULL`,
  - strict conversion warnings/errors,
  - generated-column, CHECK, FK, and rollback behavior staying unchanged,
  - unsupported shapes falling back to the normal path,
  - metadata invalidation or temporary shadowing disabling the shortcut.
- Run focused embedded statement and routed storage-engine tests.
- Run `ctest --preset storage-smoke-dev --output-on-failure`.
- Run:
  - `mylite_perf_baseline --phase=prepared-row-only-update-miss-components
    10000 1000000`
  - `mylite_perf_baseline --phase=prepared-row-only-update-components 10000
    1000000`
  - `mylite_perf_baseline --phase=prepared-assignment-update-components 10000
    1000000`
- Capture a focused sample and verify reduced time under
  `Sql_cmd_update::prepare_inner()` and `JOIN::prepare()` before claiming a
  performance win.
- Run `git diff --check` and formatting checks for touched C/C++ files.

Current validation-step verification:

- The prepared direct-update shape cache is populated only after the normal
  MyLite exact-key direct-update proof is accepted by the handler path.
- The cache is not used for direct execution yet. It is used only to retarget
  the SQL-layer exact-key proof cache after validating the current opened table
  shape, so there is no intended SQL-visible behavior change in this step.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- mariadb/sql/sql_update.cc
  mariadb/sql/sql_update.h` passed.
- `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a` passed; resulting archive size is 21,279,064 bytes.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure` passed 3/3.
- `ctest --preset storage-smoke-dev --output-on-failure` passed 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000` measured the
  prepared row-only update step at 1.567 us/op.

Current safety-net coverage extends
`test_prepared_primary_key_update_rebinds()` before enabling the shortcut. The
embedded storage-engine test now exercises repeated exact-key prepared updates
through match, no-match, bound `NULL`, unchanged-row, commuted-predicate,
additional-condition, warning, strict-conversion error recovery, CHECK error
recovery, generated-column index maintenance, transaction rollback, secondary
index, prefix index, duplicate-key, metadata reprepare after `ALTER TABLE`,
same-name temporary-table shadowing, and stable foreign-key paths. Future
rebind work should extend that test instead of replacing it, and must keep the
existing MariaDB fallback semantics visible for every unsupported shape. The
shadowing coverage includes a same-name temporary table with a different
primary-key field so stale exact-key proof reuse cannot update the wrong row
after a table-reference change.

## Acceptance Criteria

- The first supported execution still proves the shape through MariaDB's
  existing path.
- Later supported prepared row-only exact-key updates use a rebind-validated
  direct-update path without retaining stale table or join state.
- Unsupported, stale, or metadata-invalidated shapes run through the existing
  MariaDB path.
- Existing prepared update semantics and focused storage-smoke tests pass.
- Local profile evidence shows materially less time under repeated
  `Sql_cmd_update::prepare_inner()` / `JOIN::prepare()` for the supported hot
  statement.
- The archive-size delta is measured and recorded.

## Risks And Unresolved Questions

- Replacing enough of `JOIN::prepare()` to rebind fields safely may require a
  narrower MariaDB helper rather than a MyLite-only helper in `sql_update.cc`.
- `where_cond_after_prepare` is normally written by `JOIN::prepare()`. Any
  shortcut must refresh it explicitly or avoid using it until a current
  condition tree is proven fixed against the opened table.
- Prepared DML cleanup still assumes `SELECT_LEX::join == 0` before the next
  execution. The first implementation must not retain `JOIN`.
- Fallback after partial rebind must leave the statement tree clean enough for
  the normal prepare path. If this is not straightforward, the implementation
  should fail the shortcut closed before mutating reusable statement state.
- Reprepare and metadata invalidation behavior needs focused tests before this
  path can be enabled by default.
