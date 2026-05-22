# Direct Update Quick Proof Marker

## Problem

After lazy exact-update quick keys and const-key scan elision, the accepted
prepared primary-key `UPDATE` direct path still allocates and destroys a
`QUICK_EXACT_KEY_SELECT` object in `SQL_SELECT::check_quick()`.

That quick object is useful for fallback row-read execution, but the accepted
MyLite handler-direct path never resets or reads it. The handler already builds
the exact key through `ha_mylite::build_direct_update_key()` after
`ha_mylite::cond_push()` proves the same simple unique-key predicate.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` builds a
  local `SQL_SELECT`, calls `select->check_quick()`, later tries handler direct
  update, and deletes the `SQL_SELECT` before returning.
- `mariadb/sql/opt_range.cc::try_simple_update_unique_key_quick_select()` is a
  MyLite fork delta that recognizes simple single-table `UPDATE` predicates
  over one non-null single-part unique key.
- `QUICK_EXACT_KEY_SELECT` now defers key bytes until `reset()`, but the direct
  handler path still pays one per-execute object allocation and destruction only
  to prove that the predicate is unique-key exact.
- Ordinary fallback execution still needs a real quick reader before
  `init_read_record()` and `quick->reset()`.
- `Update_plan::save_explain_data_intern()` already skips detailed quick-plan
  allocation for ordinary MyLite updates unless explicit `EXPLAIN`, `ANALYZE`,
  or slow-log explain/engine detail needs it.

## Design

Teach `SQL_SELECT` to retain a MyLite exact-unique update proof separately from
the materialized quick reader:

1. When the existing simple-update matcher accepts a MyLite ordinary update and
   no explain detail is required, record the key number, value item, cost, and
   one-row estimate on `SQL_SELECT` without allocating `QUICK_EXACT_KEY_SELECT`.
2. Treat that proof as a quick plan for safe-update and unique-key planning
   decisions in `Sql_cmd_update::update_single_table()`.
3. Let the accepted MyLite direct-update path run with only the proof marker.
4. If direct update is not accepted and execution falls back to SQL-layer row
   discovery, materialize the real `QUICK_EXACT_KEY_SELECT` before any
   row-buffer or row-read path inspects quick state.
5. Keep explicit `EXPLAIN`, `ANALYZE`, non-MyLite execution, and slow-log
   explain/engine-detail paths on the existing materialized quick object so
   explain output remains backed by normal quick metadata.

All other range shapes continue through MariaDB's normal range optimizer.

## Compatibility Impact

No SQL surface changes are intended. The marker is only an internal planning
proof for the same single-row candidate set as the previous exact-key quick
object. The original `WHERE` condition remains attached to `SQL_SELECT`, and
the MyLite handler still evaluates it before updating the row.

Fallback execution materializes the same quick reader used before this slice
before entering row-buffer or row-read execution.
Unsupported engines, ordered updates, range predicates, nullable unique keys,
composite keys, and explicit explain paths keep their existing behavior.

## Single-File And Embedded Impact

No file-format, durable storage, public API, or sidecar behavior changes. This
removes SQL-layer planning allocation from a MyLite embedded hot path.

## Binary-Size Impact

The change adds small `SQL_SELECT` marker fields and branches in existing
MariaDB-derived files. It adds no dependency.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the accepted direct
  path no longer shows `QUICK_EXACT_KEY_SELECT` construction, reset, read, or
  destruction frames. `SQL_SELECT` itself still exists for the attached
  condition and ordinary statement cleanup.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates keep current affected-row and no-match behavior.
- The accepted MyLite direct-update sample avoids constructing a real quick
  object.
- Fallback execution still materializes a quick reader before row-buffer or
  row-read execution.
- Explicit `EXPLAIN UPDATE` remains on the materialized quick path.
- Prepared-update timing improves or stays within local noise.

## Risks

- Treating the marker as a quick plan must not let safe-update mode accept an
  unkeyed statement. The marker must only be set after the existing exact unique
  predicate matcher succeeds.
- Fallback execution must materialize the real quick before row-buffer or
  row-read execution can inspect quick state.
- The marker must be disabled when detailed explain output needs quick metadata.
