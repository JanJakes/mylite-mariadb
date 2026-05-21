# Prepared Unique Update Quick Fast Path

## Problem

Prepared single-row updates such as:

```sql
UPDATE perf_rows SET value = value + 1 WHERE id = ?
```

reuse the prepared statement, but MariaDB still rebuilds a full range plan for
each execution. After the recent storage-side exact-row work, local profiling
shows `Sql_cmd_update::update_single_table()` and
`SQL_SELECT::test_quick_select()` as the remaining dominant prepared primary-key
update cost.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc` calls `select->check_quick()` before considering
  handler direct-update hooks. Therefore implementing
  `handler::direct_update_rows()` in MyLite alone cannot remove the optimizer
  cost from prepared primary-key updates.
- `mariadb/sql/opt_range.h` implements `SQL_SELECT::check_quick()` as a thin
  wrapper around `SQL_SELECT::test_quick_select()`.
- `mariadb/sql/opt_range.cc` starts `SQL_SELECT::test_quick_select()` by
  deleting the previous `quick` object and rebuilding range-analysis state,
  including range `PARAM`, key-part arrays, SEL trees, and best range plans.
- `mariadb/sql/opt_range.cc` already has `get_quick_select_for_ref()`, which
  builds a `QUICK_RANGE_SELECT` directly from a fully known ref lookup. That is
  the right local pattern for a simple exact-key fast path.
- `mariadb/sql/sql_select.h` provides `store_key_item`, the same key-buffer
  value copier used by ref access. It handles bound prepared parameters with
  MariaDB field conversion rules.
- `mariadb/sql/item.h` documents that `Item_param` behaves like a literal after
  execution-time binding, so a bound prepared parameter is eligible when it is
  `const_item()`.

## Design

Add a narrow `SQL_SELECT::check_quick()` fast path before the full range
optimizer. It applies only to single-table `UPDATE` statements whose `WHERE`
condition contains an equality between one non-nullable single-part unique key
field and a known execution-time constant, including a bound prepared
parameter.

When the pattern matches:

1. Build the single key image with `store_key_item`.
2. Treat a bound `NULL` lookup value as an impossible range, not as an error.
3. Construct a `QUICK_RANGE_SELECT` directly with one `EQ_RANGE`.
4. Leave the original condition attached to `SQL_SELECT`, so any additional
   predicates still run through `select->skip_record()`.

The fast path falls back to the existing range optimizer for everything else.

## Scope

In scope:

- `UPDATE` only.
- One table.
- One non-nullable single-part unique key, including primary keys.
- `field = constant` and `constant = field`.
- Bound prepared parameters after they have a value.
- Extra `AND` predicates, with the original condition still evaluated by the
  normal update executor.

Out of scope:

- `SELECT` and `DELETE`.
- Composite keys.
- Nullable unique keys.
- `OR`, `IN`, ranges, NULL-safe `<=>`, prefix keys, full-text, spatial, hash,
  BLOB, and virtual/generated edge cases.
- Handler direct-update pushdown. That remains a separate executor/storage
  slice after planning overhead is reduced.

## Compatibility Impact

The fast path must be semantically equivalent to the existing range optimizer
for its narrow case. It chooses a single-row key access path but does not remove
or rewrite the `WHERE` condition, so MariaDB still performs expression
evaluation, warnings, affected-row behavior, assignment evaluation, triggers,
foreign-key checks, and storage-engine update calls through the existing
single-table update path.

Unsupported or ambiguous shapes continue through `test_quick_select()`.

## Single-File And Embedded Impact

No file-format, storage lifecycle, or public `libmylite` API changes are
introduced. The change reduces CPU overhead before MyLite storage is called for
hot prepared point updates.

## Binary-Size Impact

The implementation adds a small optimizer helper in an existing MariaDB-derived
translation unit and no dependencies. Binary-size impact should be negligible
and covered by the existing embedded archive build.

## Test Plan

- Add routed-storage coverage for prepared primary-key update statements that
  are reset and rebound repeatedly, including `field = ?`, `? = field`, and an
  extra `AND` predicate.
- Cover a bound `NULL` primary-key value and assert it updates no rows and does
  not accidentally update key `0`.
- Run storage-smoke `libmylite` tests.
- Build the MariaDB embedded storage-smoke archive.
- Run `git diff --check` and `git clang-format --diff` on touched source files.
- Sample `mylite_perf_baseline --phase=prepared-updates` before commit.

## Acceptance Criteria

- Simple prepared primary-key updates still produce correct affected rows and
  values across reset/re-execute loops.
- Bound `NULL` values for a non-nullable unique key produce no match.
- Existing routed storage-engine tests pass.
- The prepared update benchmark improves or stays neutral locally.
- The diff remains a narrow optimizer fast path with fallback to existing range
  planning.

## Risks

- MariaDB condition trees are broad; the matcher intentionally supports only a
  small equality subset and falls back otherwise.
- Conversion warning behavior must stay owned by MariaDB field conversion. The
  implementation uses `store_key_item` rather than custom serialization.
- The first slice does not remove executor-row-update overhead; handler
  direct-update remains useful later, but not as the first fix for the observed
  profile.
