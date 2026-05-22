# MyLite Direct Update Compare Readiness Cache

## Problem

Prepared primary-key `UPDATE` loops now reach the MyLite handler direct-update
path for the accepted stable-key shape, but the handler still recomputes
`records_are_comparable(table)` immediately before each row comparison.

That readiness check depends on table/versioning flags, handler partial-read
flags, and the statement read/write bitmaps already fixed before
`direct_update_rows_init()` accepts the direct path. Repeating it inside
`direct_update_rows()` adds unnecessary work to the hot single-row mutation
path.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::update_single_table()` computes
  `records_are_comparable(table)` once before the normal row-update loop and
  reuses the result for every candidate row.
- `mariadb/sql/sql_update.cc::records_are_comparable()` checks
  `TABLE::versioned()`, `HA_PARTIAL_COLUMN_READ`, and whether the update
  `write_set` is covered by the current `read_set`.
- `mariadb/storage/mylite/ha_mylite.cc::direct_update_rows_init()` runs after
  SQL has marked update read/write columns and after the accepted exact-key
  direct-update proof, field list, and value list have been pushed to the
  handler.
- `mariadb/storage/mylite/ha_mylite.cc::direct_update_rows()` applies the row
  condition, fills the new record buffer, then uses
  `!records_are_comparable(table) || compare_record(table)` to preserve
  MariaDB unchanged-row semantics.

## Design

1. Add a handler-owned boolean for the accepted direct-update statement's
   compare readiness.
2. Clear that boolean whenever the direct-update state is cleared or replaced.
3. Set the boolean in `direct_update_rows_init()` after all direct-update gates
   pass.
4. Use the cached boolean in `direct_update_rows()` and keep
   `compare_record(table)` itself unchanged.

This mirrors the normal MariaDB update loop's "compute once, reuse while
executing" pattern while keeping the optimization inside the handler-owned
direct route.

## Affected MariaDB Subsystems

- MyLite storage handler direct-update state in
  `mariadb/storage/mylite/ha_mylite.{h,cc}`.

No parser, optimizer, SQL diagnostics, catalog, file-format, or wire-protocol
behavior changes.

## Compatibility Impact

Affected-row and unchanged-row behavior should remain the same. The cached
value only controls whether it is valid to call MariaDB's existing
`compare_record(table)` helper; it does not change row equality rules.

Broader update shapes that do not pass MyLite `direct_update_rows_init()` keep
the existing MariaDB execution path.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No engine routing-policy change. The slice only affects a handler-internal
optimization after SQL has already selected a MyLite routed table and the
accepted exact-key direct-update path.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds one boolean field and a few assignments. It adds no dependency
and should have neutral archive-size impact, measured through the
storage-smoke embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Reuse existing prepared primary-key update coverage for match, no-match,
  NULL-key, unchanged-row, additional-condition, `UPDATE IGNORE`, and stable
  secondary-index cache behavior.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates preserve affected-row, no-match, NULL-key, unchanged
  row, and warning behavior.
- The direct-update handler computes compare readiness during init and reuses
  it during row mutation.
- No new sidecars, public API changes, file-format changes, or dependencies are
  introduced.

## Risks And Unresolved Questions

- The cached value must not survive a replaced direct-update proof or field
  list. Clearing it with direct-update state changes keeps stale `true` values
  from reaching `compare_record(table)` under a later incompatible statement.
- This removes a small repeated check. The larger remaining cost is SQL
  prepared-DML re-analysis, which needs a separate source-backed design.
