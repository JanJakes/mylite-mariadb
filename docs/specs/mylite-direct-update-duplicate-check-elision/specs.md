# MyLite Direct Update Duplicate Check Elision

## Problem

Prepared primary-key `UPDATE` samples now spend most time in MyLite handler
row mutation. Within `ha_mylite::update_row()`, the changed-index path still
enters duplicate-key checking even for accepted direct updates.

The accepted MyLite direct-update route already rejects updates that change any
unique key part. Duplicate-key checks are therefore unnecessary for that route:
the only accepted key changes are non-unique index maintenance changes.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::direct_update_rows_init()` calls
  `mylite_update_fields_change_direct_unsafe_key()` before accepting the
  handler direct-update route.
- `mylite_update_fields_change_direct_unsafe_key()` treats any updated
  `HA_NOSAME` key part as unsafe by returning an error through
  `mylite_field_is_direct_unsafe_key_part()`, causing direct-update fallback.
- The same helper only reports non-unique key changes through
  `out_changes_key`, where direct update remains allowed after foreign-key
  presence checks.
- `ha_mylite::update_row()` performs duplicate checks only after new index
  entries are prepared, and duplicate checks only matter for changed unique
  keys.
- `mariadb/sql/sql_base.cc::fill_record()` may evaluate `ON UPDATE` default
  functions for fields that were not explicitly assigned, so the direct route
  must keep duplicate checking if any unique key part has an update-default
  function.

## Design

1. Add a handler-local `direct_update_row_in_progress` flag.
2. Set the flag only around the `update_row()` call made by
   `direct_update_rows()`.
3. During `direct_update_rows_init()`, compute whether duplicate checks can be
   skipped by proving that no unique key part is in the write set and no unique
   key part has an `ON UPDATE` default function.
4. Skip duplicate-key checking in `update_row()` only when both the in-progress
   flag and the duplicate-check skip proof are set.
5. Keep normal direct SQL, fallback prepared updates, inserts, volatile rows,
   foreign-key checks, CHECK constraints, auto-increment handling, row payload
   updates, and index maintenance on their existing paths.

This keeps the optimization bounded to the direct route whose init gate has
already proved that unique-key-changing updates are not accepted.

## Affected MariaDB Subsystems

- MyLite storage handler direct-update and row-update code in
  `mariadb/storage/mylite/ha_mylite.{h,cc}`.

No parser, optimizer, catalog, file-format, public API, or wire-protocol
behavior changes.

## Compatibility Impact

Duplicate-key behavior for supported direct updates is unchanged because the
optimized subpath proves that unique key values cannot change explicitly or via
`ON UPDATE` defaults. Updates that might change unique keys continue to fall
back before direct execution or keep normal duplicate-key checking.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No engine routing-policy change. This only affects row mutation after an
ordinary MyLite routed table has passed the direct-update init gate.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds one boolean field and a guarded branch. It adds no dependency
and should have neutral archive-size impact, measured through the
storage-smoke embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Reuse existing prepared primary-key update coverage, including stable
  secondary-index updates, failed duplicate-key paths outside the direct route,
  and statement rollback tests.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Direct prepared point updates preserve affected-row, no-match, unchanged
  row, warning, and secondary-index maintenance behavior.
- Unique-key-changing updates continue to use fallback paths with duplicate-key
  checking.
- No new sidecars, public API changes, file-format changes, or dependencies are
  introduced.

## Risks And Unresolved Questions

- The skip is only valid while `direct_update_rows()` is calling
  `update_row()` and after unique-key immutability has been proved. Persistent
  handler flags must therefore be set and cleared carefully.
- This does not remove the remaining secondary-index serialization or storage
  rewrite cost for non-unique index changes.
