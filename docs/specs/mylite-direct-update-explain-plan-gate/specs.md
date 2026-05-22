# MyLite Direct Update Explain Plan Gate

## Problem

After the update explain-detail gate, hot prepared primary-key `UPDATE` samples
still show `Update_plan::save_explain_update_data()` allocating an
`Explain_update` node for ordinary MyLite direct-update execution. The earlier
slice already skips expensive key-list, quick-info, and MRR detail collection,
but the remaining node allocation and command-tracker setup are still
unobservable for supported MyLite embedded APIs when the statement is not
`EXPLAIN`, `ANALYZE`, or slow-log explain/engine output.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` creates an
  `Explain_update` node before trying handler direct update, then starts the
  node's command tracker.
- The direct-update branch does not use row-read trackers, buffer trackers, or
  filesort trackers, and exits through `update_end` before the row-scan
  `ANALYZE_STOP_TRACKING()` site.
- Non-direct update paths still use `explain->tracker`, `explain->buf_tracker`,
  or filesort tracker state while scanning, buffering, or sorting rows.
- `mariadb/sql/sql_delete.cc::Update_plan::save_explain_data_intern()` already
  keeps full plan detail for non-MyLite execution, explicit `EXPLAIN`,
  `ANALYZE`, and slow-log explain/engine verbosity.
- MyLite's server-surface policy does not treat `SHOW EXPLAIN` as a supported
  embedded API surface, while explicit routed `EXPLAIN UPDATE` remains
  supported.

## Design

Gate the remaining update explain-plan allocation for accepted MyLite
direct-update execution:

1. Add a local helper that returns true when the update path needs an
   `Explain_update` node:
   - non-MyLite execution,
   - explicit `EXPLAIN`,
   - `ANALYZE`,
   - slow-log explain or engine verbosity.
2. Keep the existing early `save_explain_update_data()` call when the helper
   says explain/analysis/logging needs the node. Otherwise, defer ordinary
   MyLite execution's node creation until after the direct-update probe.
   Allocate the node if the direct update was not accepted.
3. Guard `ANALYZE_STOP_TRACKING()` for the no-node direct path. The no-node path
   is only entered when `ANALYZE` is false, so no timing data is lost.
4. Keep explicit `EXPLAIN UPDATE` on the existing early
   `produce_explain_and_leave` path.

This does not change range optimization, direct-update eligibility, condition
evaluation, row mutation, or diagnostics.

## Affected MariaDB Subsystems

- Single-table `UPDATE` execution in `mariadb/sql/sql_update.cc`.
- Existing update explain node creation through
  `mariadb/sql/sql_delete.cc::Update_plan::save_explain_update_data()`.

No parser, handler, storage, catalog, file-format, or wire-protocol behavior
changes.

## Compatibility Impact

Explicit `EXPLAIN UPDATE`, `ANALYZE UPDATE`, non-MyLite MariaDB execution, and
slow-log explain/engine output keep allocating and filling the normal
`Explain_update` node. Ordinary MyLite direct updates do not expose that node
through supported MyLite APIs.

Non-direct MyLite updates keep the normal node because scan, buffer, and
filesort execution still use its trackers.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No routing-policy change. The optimization applies after the MyLite handler
accepts direct-update execution for the existing routed table shape.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds one small branch and helper. It adds no dependency. Archive-size
impact should be neutral to negligible and measured through the storage-smoke
embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the accepted direct
  path no longer reports `Update_plan::save_explain_update_data()`.
- Reuse existing routed explicit `EXPLAIN UPDATE` coverage to ensure explicit
  plan output remains intact.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates preserve affected-row, no-match, unchanged-row, and
  warning-count behavior.
- Explicit routed `EXPLAIN UPDATE` remains covered and keeps expected key
  output.
- The hot prepared-update sample avoids `Update_plan::save_explain_update_data()`
  on the accepted direct-update path.

## Risks And Unresolved Questions

- Skipping the node outside the direct-update path would break scan, buffer, or
  filesort trackers. The allocation must therefore remain mandatory for
  non-direct updates.
- If MyLite later supports `SHOW EXPLAIN` as an embedded API surface, ordinary
  execution may need a supported mechanism to opt back into runtime explain
  node creation.
