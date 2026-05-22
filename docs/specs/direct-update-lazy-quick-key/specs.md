# Direct Update Lazy Quick Key

## Problem

The prepared primary-key `UPDATE` hot path now reaches MyLite's handler direct
update hook, but the SQL-layer simple-update quick shortcut still materializes
an exact key during `SQL_SELECT::check_quick()`.

Local sampling after handler-side key-field reuse shows the remaining
`Field::new_key_field()` and `store_key_item::copy_inner()` samples under
`SQL_SELECT::check_quick()`, not under `ha_mylite::direct_update_rows()`.
For the direct handler path, that key image is not used for row retrieval.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` builds a
  per-execution `SQL_SELECT`, calls `select->check_quick()`, then later decides
  whether the handler direct update path is available.
- The MyLite fork delta in
  `mariadb/sql/opt_range.cc::try_simple_update_unique_key_quick_select()` keeps
  the accepted shape narrow: single-table `UPDATE`, one non-nullable single-part
  unique key, no filled-at-execution table, and a simple equality predicate.
- `mariadb/sql/opt_range.cc::make_simple_update_unique_key_quick_select()`
  currently serializes the lookup key immediately with `store_key_item`, then
  creates `QUICK_EXACT_KEY_SELECT`.
- `mariadb/sql/sql_update.cc::update_single_table()` still calls
  `select->quick->reset()` and `init_read_record()` at `update_begin` before
  invoking `handler::ha_direct_update_rows()`, even though the direct handler
  path does not read rows through `READ_RECORD`.
- `mariadb/sql/records.cc::end_read_record()` tolerates a default-constructed
  `READ_RECORD`; it only ends handler state when `info->table` was initialized.

## Design

Defer exact-key materialization in MyLite's simple-update quick object until
the quick reader is actually reset for row retrieval.

1. Add an exact-key quick constructor mode that stores the `THD`, key number,
   value item, and table metadata instead of a prebuilt key image.
2. Move the existing `store_key_item` serialization into a private quick helper
   invoked from `QUICK_EXACT_KEY_SELECT::reset()`.
3. Cache the serialized key inside the quick object after a successful reset.
4. Preserve the early bound-NULL impossible-range check in
   `make_simple_update_unique_key_quick_select()` so repeated prepared
   executions with `NULL` still become no-row updates without touching storage.
5. In `Sql_cmd_update::update_single_table()`, move the handler direct-update
   execution before `select->quick->reset()` and `init_read_record()`, after the
   statement warning/cut-field state has been initialized.

Fallback execution that actually scans through the quick object will still
materialize the key before the first `ha_index_read_map()` call. Direct handler
updates will skip that unused SQL-layer key build and read cursor setup.

## Affected Subsystems

- MariaDB SQL range quick path in `mariadb/sql/opt_range.cc`.
- Single-table `UPDATE` execution setup in `mariadb/sql/sql_update.cc`.
- MyLite storage handler behavior is not changed by this slice.

## Compatibility Impact

No SQL surface or accepted predicate shape changes. The original `WHERE`
condition remains attached to `SQL_SELECT`, and `ha_mylite::direct_update_rows()`
continues to evaluate the pushed condition, assignments, affected-row counts,
warnings, no-op updates, and rollback behavior.

If the direct update path is unavailable, the quick object still builds the same
serialized key and reads through `ha_index_read_map()` as before.

## Single-File And Embedded Impact

No file-format, durable storage, public API, or sidecar behavior changes. This
is SQL-layer runtime scratch work.

## Binary-Size Impact

The change extends an existing MyLite quick-select helper in an existing
translation unit and adds no dependencies. Size impact should be negligible and
will be covered by the storage-smoke archive measurement.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build the storage-smoke embedded storage-engine test and performance tool.
- Run the focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm
  `Field::new_key_field()` / `store_key_item::copy_inner()` no longer appear in
  the accepted direct-update execution sample.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared primary-key updates keep current functional behavior, including
  rebound integer parameters, reversed equality, additional `AND` predicates,
  bound `NULL`, no-op updates, and affected-row reporting.
- The accepted direct-update sample no longer shows SQL-layer key-field clone
  allocation under `SQL_SELECT::check_quick()`.
- Prepared-update timing improves or stays within local noise.

## Risks

- Moving direct update before `init_read_record()` must not skip statement
  warning, truncation, default-field, condition-pushdown, or cleanup state that
  the direct handler path depends on.
- Fallback quick scans must still materialize the exact key before calling
  `ha_index_read_map()`.
- `EXPLAIN UPDATE` must remain on the existing plan-detail path and must not
  rely on lazy key materialization.
