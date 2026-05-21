# Prepared Exact-Key Quick Select

## Problem

The prepared unique-key `UPDATE` fast path avoids MariaDB's full range
optimizer rebuild for statements such as:

```sql
UPDATE perf_rows SET value = value + 1 WHERE id = ?
```

However, the fast path still builds a generic `QUICK_RANGE_SELECT` with one
`EQ_RANGE`. Local profiles after the storage-side update rewrites show
remaining time in `QUICK_RANGE_SELECT::QUICK_RANGE_SELECT()`,
`QUICK_RANGE_SELECT::reset()`, `QUICK_RANGE_SELECT::get_next()`, and
`QUICK_RANGE_SELECT::~QUICK_RANGE_SELECT()`. That machinery is designed for
general range scans and MRR, while this MyLite fast path needs one exact
non-null unique-key probe.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` creates a
  local `SQL_SELECT` with `make_select()`, uses `select->check_quick()`, then
  deletes the select at execution end. Therefore caching `select->quick` across
  prepared executions is not a suitable optimization.
- `mariadb/sql/opt_range.cc::try_simple_update_unique_key_quick_select()` is a
  MyLite fork delta that handles only simple single-table `UPDATE` predicates
  over one non-nullable single-part unique key and falls back to MariaDB's
  normal `test_quick_select()` for all other shapes.
- `mariadb/sql/opt_range.cc::get_quick_select_for_exact_key()` currently
  creates a `QUICK_RANGE_SELECT`, allocates a dynamic range array, allocates one
  `QUICK_RANGE`, fills one `KEY_PART`, calls
  `handler::multi_range_read_info()`, and later executes through
  `handler::multi_range_read_init()` / `handler::multi_range_read_next()`.
- `mariadb/sql/records.cc::init_read_record()` only needs a `QUICK_SELECT_I`
  object whose `reset()` and `get_next()` implement the quick read contract.
  `records.cc::end_read_record()` intentionally leaves `rr_quick` handler
  cleanup to `quick->range_end()` / the quick destructor.
- `mariadb/sql/handler.h` exposes `handler::ha_index_read_map()`, which is the
  direct handler call for one exact key read after `ha_index_init()`.

## Design

Replace the existing fast path's generic one-range `QUICK_RANGE_SELECT` with a
small exact-key `QUICK_SELECT_I` implementation local to `opt_range.cc`.

The new quick object:

1. Owns the serialized full key image inline.
2. Initializes the handler on the target index in `reset()`.
3. On the first `get_next()`, calls `ha_index_read_map()` with
   `HA_READ_KEY_EXACT`.
4. Converts no-match handler results to `HA_ERR_END_OF_FILE`, matching the
   quick-read contract used by `records.cc`.
5. Ends the index cursor in `range_end()` and from the destructor.

The existing predicate matcher, key serialization with `store_key_item`, NULL
handling, cost estimates, and fallback behavior stay unchanged.

## Scope

In scope:

- The existing simple prepared/direct `UPDATE` fast path over one non-nullable
  single-part unique key.
- Primary keys and non-null unique secondary keys accepted by the existing
  matcher.
- `field = constant`, `constant = field`, and additional `AND` predicates that
  the normal update executor still evaluates through `select->skip_record()`.

Out of scope:

- Cross-execution plan caching.
- Direct handler update pushdown.
- `SELECT`, `DELETE`, composite keys, nullable unique keys, ranges, `OR`, `IN`,
  NULL-safe equality, and prefix-key special cases.

## Compatibility Impact

The row candidate set is the same as the previous `EQ_RANGE` quick path for the
accepted shape: at most one full unique-key row. The original `WHERE` condition
is still attached to `SQL_SELECT`, so MariaDB continues to own expression
evaluation, conversion warnings, affected-row behavior, triggers, generated
columns, foreign-key checks, and storage-engine update calls.

Unsupported shapes still use the existing range optimizer.

## Single-File And Embedded Impact

No file-format, storage lifecycle, public API, or durable sidecar behavior
changes. This is a SQL-layer execution-path optimization before MyLite storage
is called.

## Binary-Size Impact

The change adds one small quick-select class in an existing MariaDB-derived
translation unit and no dependencies. Binary-size impact should be negligible.

## Test Plan

- Reuse the routed-storage prepared primary-key update coverage for repeated
  reset/rebind execution, reversed equality, bound NULL, and extra `AND`
  predicates.
- Build the MariaDB storage-smoke embedded archive target affected by
  `opt_range.cc`.
- Build the storage-smoke `libmylite` and performance targets.
- Run storage-smoke routed tests.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.
- Sample `mylite_perf_baseline --phase=prepared-updates`.

## Acceptance Criteria

- Prepared primary-key updates keep the current functional behavior and
  affected-row counts.
- Bound NULL values still update no rows.
- The fast path no longer constructs `QUICK_RANGE_SELECT` for accepted exact
  unique-key updates.
- Focused builds and routed-storage tests pass.
- The local prepared-update benchmark improves or stays neutral.

## Risks

- `ha_index_read_map()` must return no-match as end-of-quick-scan rather than
  an SQL error. The implementation must normalize not-found results.
- General range explain output expects quick objects to report key names and
  used key lengths. The exact-key quick must preserve those virtual methods.
- Direct handler update pushdown remains a separate larger optimization because
  it must account for MariaDB update expression semantics and statement effects.
