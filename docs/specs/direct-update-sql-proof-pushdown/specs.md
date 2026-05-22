# Direct Update SQL Proof Pushdown

## Problem

MyLite prepared primary-key `UPDATE` execution now keeps the SQL-layer
unique-key proof as a lightweight `SQL_SELECT` marker. The accepted direct
handler path still calls `ha_mylite::cond_push()`, which walks the `WHERE`
condition again to rediscover the same single-part exact unique-key predicate.

That duplicate walk is visible in local prepared-update samples after the quick
allocation was removed. The handler still needs the original condition for
MariaDB expression evaluation before updating the row, but it does not need to
reprove the key shape when the SQL layer already used the same predicate matcher
to mark the direct-update candidate.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` decides
  whether to attempt handler direct update and currently asks the handler to
  accept `select->cond` through `cond_push()`.
- `mariadb/sql/opt_range.cc::try_simple_update_unique_key_quick_select()`
  recognizes simple single-table `UPDATE` predicates over one non-null
  single-part unique key and now records an exact-key marker on `SQL_SELECT`
  for ordinary MyLite direct-update candidates.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::cond_push()` repeats a
  MyLite-specific condition walk through
  `mylite_find_direct_update_exact_key()` before storing
  `direct_update_condition`, `direct_update_key_value`, and
  `direct_update_key_number`.
- `ha_mylite::direct_update_rows()` still evaluates
  `direct_update_condition->val_bool()` after reading the candidate row, so
  skipping the handler proof walk must not remove the final SQL condition
  check.

## Design

Pass the SQL-layer exact-key proof to the handler through the existing
`handler::info_push()` extension point:

1. Add a MyLite-specific `INFO_KIND_MYLITE_UPDATE_EXACT_KEY` payload carrying
   the original condition, key number, and value item.
2. When `Sql_cmd_update::update_single_table()` sees the MyLite
   `SQL_SELECT` exact-key marker, push that proof before falling back to
   `cond_push()`.
3. Let `ha_mylite::info_push()` accept the proof only if the key is also
   supported by the handler-direct raw-key subset. Unsupported key shapes return
   `HA_ERR_WRONG_COMMAND` and keep the existing `cond_push()` fallback.
4. Preserve `table->file->pushed_cond` and `cond_pop()` semantics for the
   accepted proof path, because MariaDB still owns condition lifetime and
   handler cleanup.
5. Keep all non-marker predicates, non-MyLite handlers, explain-detail paths,
   and unsupported direct-update shapes on the existing `cond_push()` path.

The handler keeps evaluating the original condition before applying the update,
and `direct_update_rows_init()` retains the existing table-shape,
constraint-sensitive, key-changing, FK, view, and primary-file guards.

## Compatibility Impact

No SQL result or public API changes are intended. The optimization only
short-circuits a duplicated internal proof for a predicate already accepted as a
single-row exact unique-key update candidate.

Fallback to normal SQL-layer row discovery remains unchanged when the handler
does not accept the pushed proof. MariaDB expression evaluation and condition
truth remain authoritative before the direct row update is applied.

## Single-File And Embedded Impact

No file-format, durable storage, sidecar, transaction, or public embedded API
changes. The slice only reduces SQL-to-handler control-plane work for an
accepted embedded hot path.

## Binary-Size Impact

The change adds one small handler info kind, one payload struct, and narrow
branches in existing MariaDB-derived and MyLite handler files. It adds no
dependency.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the accepted direct
  path no longer shows `ha_mylite::cond_push()` or the MyLite condition-proof
  helper frames.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates keep current affected-row and no-match behavior.
- The accepted MyLite direct-update sample avoids the duplicate handler
  condition-proof walk.
- Unsupported direct-update key shapes still fall back through `cond_push()` or
  normal row-read execution.
- Explicit `EXPLAIN UPDATE` remains outside the marker proof path.

## Risks

- The handler must not trust a SQL marker for key shapes it cannot update
  directly; it must revalidate the key against `mylite_direct_update_key_is_supported()`.
- Skipping `cond_push()` must not skip final `WHERE` evaluation in
  `direct_update_rows()`.
- `pushed_cond` / `cond_pop()` bookkeeping must match the existing accepted
  `cond_push()` path.
