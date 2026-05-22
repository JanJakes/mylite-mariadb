# MyLite Direct Update Explain Query Gate

## Problem

After accepted direct updates skip the remaining `Explain_update` node,
prepared primary-key `UPDATE` samples still show `JOIN::prepare()` allocating
the outer `Explain_query` object through `create_explain_query()`.

That object is required for explicit `EXPLAIN`, `ANALYZE`, slow-log explain or
engine detail, and non-direct execution paths that attach row, buffer, or
filesort trackers. It is not observable for the ordinary MyLite direct-update
path that never creates an update explain node.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_select.cc::JOIN::prepare()` eagerly calls
  `create_explain_query_if_not_exists()` for runtime execution outside
  `CONTEXT_ANALYSIS_ONLY_PREPARE`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` now
  defers `Explain_update` node creation for ordinary MyLite execution until
  after the handler direct-update probe.
- Non-direct update execution still dereferences explain trackers while
  scanning, buffering, sorting, or producing explicit explain output.
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute()` tolerates
  `thd->lex->explain == NULL`; it only transfers or deletes the current
  execution explain object when one exists.

## Design

1. Add a MyLite-only helper in `JOIN::prepare()` that skips eager
   `Explain_query` creation only for ordinary `SQLCOM_UPDATE` execution when
   schema hooks are active and no explicit `EXPLAIN`, `ANALYZE`, or slow-log
   explain/engine detail can observe it.
2. Before `Sql_cmd_update::update_single_table()` saves an `Explain_update`
   node on any non-direct or explain-observable path, call
   `create_explain_query_if_not_exists()`.
3. Keep direct-update execution with no explain node on the no-object path.
4. Leave SELECT, multi-table UPDATE, DELETE, INSERT, explicit `EXPLAIN`,
   `ANALYZE`, non-MyLite execution, and slow-log detail paths on MariaDB's
   existing eager or required explain creation.

This is a narrow allocation gate. It does not change table opening, locking,
range optimization, condition evaluation, direct-update eligibility, row
mutation, diagnostics, or prepared-statement metadata validation.

## Affected MariaDB Subsystems

- `JOIN::prepare()` explain-object setup in `mariadb/sql/sql_select.cc`.
- Single-table `UPDATE` explain-node creation in `mariadb/sql/sql_update.cc`.

No parser, handler, storage, catalog, file-format, or wire-protocol behavior
changes.

## Compatibility Impact

Explicit `EXPLAIN UPDATE`, `ANALYZE UPDATE`, non-MyLite MariaDB execution, and
slow-log explain/engine output keep normal explain objects. Ordinary MyLite
direct updates do not expose the skipped object through supported embedded APIs.

Fallback non-direct MyLite updates create the explain object before creating
the `Explain_update` node, preserving tracker behavior.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No routing-policy change. The gate only applies after SQL parsing has selected
ordinary single-table `UPDATE` execution and before MyLite direct-update
eligibility is probed.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds one small helper and two lazy explain creation calls. It adds no
dependency. Archive-size impact should be neutral to negligible and measured
through the storage-smoke embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the accepted direct
  path no longer reports `create_explain_query()`.
- Reuse existing routed explicit `EXPLAIN UPDATE` coverage to ensure explicit
  plan output remains intact.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates preserve affected-row, no-match, unchanged-row, and
  warning-count behavior.
- Explicit routed `EXPLAIN UPDATE` remains covered and keeps expected key
  output.
- The hot prepared-update sample avoids `create_explain_query()` on the
  accepted direct-update path.

## Risks And Unresolved Questions

- Skipping the outer object for fallback execution would break update explain
  tracker setup. `Sql_cmd_update::update_single_table()` must therefore create
  it lazily before any `Explain_update` node creation.
- This does not solve repeated prepared-DML context analysis. That larger
  change needs a separate design because MariaDB currently unprepares DML
  command objects after each execution.
