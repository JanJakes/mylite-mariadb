# Direct Update Empty Guard Elision

## Problem

The prepared primary-key update hot path now reaches MyLite's accepted
`ha_mylite::direct_update_rows()` path reliably. Sampling the local
`prepared-updates` benchmark still shows the row loop paying MariaDB handler
guard calls whose common table shape proves they cannot do useful work:

- `handler::prepare_for_modify(true, true)` only opens hidden vector indexes or
  prepares in-server long unique / period unique constraint helpers.
- `TABLE::verify_constraints(false)` only evaluates CHECK constraints after a
  cheap THD error check.
- `TABLE::hlindexes_on_update()` only updates hidden vector indexes.

The benchmark table has no hidden indexes, no in-server unique constraint
helpers, and no CHECK constraints. The accepted direct-update init gate already
rejects table shapes that need in-server update constraints, so the per-row path
can avoid the remaining empty guards without changing SQL behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::prepare_for_modify()` opens hidden indexes
  and prepares long-unique / period-unique helpers only when those table-share
  flags are present.
- `mariadb/sql/table.cc::TABLE::verify_constraints()` first checks the THD
  error state and then evaluates `check_constraints` only when that pointer is
  non-null and check enforcement is enabled.
- `mariadb/sql/sql_base.cc::TABLE::hlindexes_on_update()` is a no-op unless
  `table->hlindex` is present and in use.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows_init()`
  rejects MyLite direct updates for table shapes that need in-server update
  constraints through `mylite_table_needs_inserver_update_constraints()`.
- `ha_mylite::direct_update_rows()` still calls those guard helpers for every
  accepted direct update row.

## Design

- In `ha_mylite::direct_update_rows()`, call `prepare_for_modify(true, true)`
  only when the table share reports hidden indexes. Long unique and period
  unique shapes are already rejected during direct-update initialization.
- Call `TABLE::verify_constraints(false)` only when the table has CHECK
  constraints. Preserve the existing THD error check for the no-CHECK case.
- Call `TABLE::hlindexes_on_update()` only when the table share reports hidden
  indexes.
- Keep the existing fallback, CHECK, hidden-index, FK, duplicate-key, affected
  rows, and unchanged-row behavior unchanged.

## Compatibility Impact

No SQL-visible behavior change is intended. Tables with CHECK constraints still
evaluate them. Tables with hidden indexes still run hidden-index preparation and
update hooks. Accepted ordinary MyLite direct updates avoid only guards that are
provably empty for the table shape.

## Single-File And Lifecycle Impact

No storage file, journal, sidecar, lock, checkpoint, or recovery change.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Test Plan

- Keep existing routed CHECK and generated-column update diagnostics passing,
  including a prepared exact-key CHECK update failure.
- Keep embedded storage-engine smoke passing.
- Run the prepared update performance phase before and after the change.
- Run formatting and whitespace checks.

## Acceptance Criteria

- Accepted direct updates over ordinary MyLite tables avoid per-row empty
  prepare/constraint/hidden-index guard calls.
- CHECK-constrained direct updates still reject violating rows through existing
  embedded storage-engine tests.
- Storage-smoke tests pass.
- The local prepared-update benchmark does not regress.

## Risks

- Hidden vector indexes are currently outside MyLite's supported default
  profile, but the handler must still preserve the upstream hook if such a
  table shape reaches this path.
- `TABLE::verify_constraints()` also checks THD error state. The no-CHECK fast
  path must keep an explicit THD error check after record filling and
  comparison.
