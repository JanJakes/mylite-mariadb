# Direct Update Const-Key Scan Elision

## Problem

After lazy exact-update quick keys, the prepared primary-key `UPDATE` hot path
still spends visible SQL-layer time in `TABLE::update_const_key_parts()`.

That helper scans every key part and walks the `WHERE` condition to mark key
parts equal to constants. In the accepted prepared point-update benchmark there
is no `ORDER BY`, and the previously built unique-key quick already tells
`Sql_cmd_update::update_single_table()` that the statement has at most one
candidate row.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` calls
  `table->update_const_key_parts(conds)` before `simple_remove_const(order,
  conds)` and before the unique-key quick branch.
- `mariadb/sql/table.cc::TABLE::update_const_key_parts()` clears
  `TABLE::const_key_parts`, then scans all table keys and calls
  `const_expression_in_where()` for each user-defined key part when a condition
  exists.
- `mariadb/sql/sql_select.cc::test_if_order_by_key()` uses
  `TABLE::const_key_parts` to skip key parts that are constants in the `WHERE`
  clause while reasoning about `ORDER BY`.
- In the no-`ORDER BY`, unique-key quick branch,
  `Sql_cmd_update::update_single_table()` does not need that condition-derived
  order-planning state: it sets `need_sort= FALSE`, `query_plan.index= MAX_KEY`,
  and `used_key_is_modified= FALSE`.

## Design

In single-table `UPDATE`, skip the full `update_const_key_parts(conds)` walk
when all of the following are true:

1. There is no `ORDER BY`.
2. `select->quick` exists.
3. `select->quick->unique_key_range()` is true.

Still clear `TABLE::const_key_parts` in that branch through a small inline
table helper, preserving stale-state cleanup without entering the full
condition-walking helper.

All other update shapes keep the existing behavior.

## Compatibility Impact

No SQL surface changes. The optimization is limited to no-order unique-key
quick updates where order simplification and ordered-index selection do not use
`const_key_parts`.

Unsupported or broader shapes, including ordered updates, non-unique ranges,
full scans, and filesort/io-buffer update paths, keep the existing full scan.

## Single-File And Embedded Impact

No file-format, durable storage, public API, or sidecar behavior changes. This
is SQL-layer planning scratch work.

## Binary-Size Impact

The change adds only a small branch in an existing MariaDB-derived translation
unit and no dependencies.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build the storage-smoke embedded storage-engine test and performance tool.
- Run the focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the accepted no-order
  direct-update sample no longer enters `TABLE::update_const_key_parts()` or
  `const_expression_in_where()`.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates keep current functional behavior.
- Ordered or non-unique update planning is untouched.
- The accepted no-order direct-update sample no longer shows
  `TABLE::update_const_key_parts()` condition walking.
- Prepared-update timing improves or stays within local noise.

## Risks

- `TABLE::const_key_parts` must still be cleared to avoid stale statement state.
- The elision must not apply to ordered updates because `ORDER BY` planning
  consults `const_key_parts`.
- The elision must not apply to non-unique quick ranges where modified key
  detection and ordered index choice can still need the broader key-part map.
