# MyLite Direct Update Key Change Mask

## Problem

Accepted direct updates still recompute per-index changeability during
`ha_mylite::update_row()`. That repeats `write_set` and key-part metadata walks
after `direct_update_rows_init()` has already accepted a stable statement
shape.

For prepared point updates, the write set is fixed for the statement execution.
The handler can cache which indexes may change once during direct-update init
and reuse that mask while preserving unchanged index entries and preparing the
changed-entry bitmap.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::direct_update_rows_init()` runs after
  SQL has fixed the update field list and table read/write bitmaps.
- `mylite_key_fields_may_change()` decides whether a key has any key-part
  field in `TABLE::write_set`, conservatively treating virtual or malformed key
  parts as changeable.
- `mylite_update_preserves_all_index_entries()` currently compares every
  supported index key between old and new records.
- `mylite_prepare_index_entry_changes()` already skips old-key comparisons for
  indexes whose fields cannot change, but it recomputes that proof for each
  row update.

## Design

1. Add a handler-local `direct_update_key_may_change[MAX_KEY]` mask.
2. Clear the mask whenever direct-update state is cleared or replaced.
3. Fill the mask in `direct_update_rows_init()` after all direct-update gates
   pass.
4. During `update_row()`, pass the cached mask only while the call is being
   made from `direct_update_rows()`.
5. Use the mask to skip preserved-index comparisons and changed-entry
   rediscovery for indexes that cannot change.

The actual old/new key comparison still runs for every index marked as possibly
changeable, so unchanged-row and unchanged-index behavior remains data-driven.

## Affected MariaDB Subsystems

- MyLite storage handler direct-update and row-update code in
  `mariadb/storage/mylite/ha_mylite.{h,cc}`.

No parser, optimizer, catalog, file-format, public API, or wire-protocol
behavior changes.

## Compatibility Impact

Affected-row behavior and index maintenance semantics are unchanged. The mask
only lets the handler skip keys that cannot be written by the accepted
statement; keys that may change are still compared using the existing old/new
record logic.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No engine routing-policy change. This only affects handler-internal work after
an ordinary MyLite routed table has passed the direct-update init gate.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds one small fixed-size handler mask and a few branches. It adds no
dependency and should have neutral to small archive-size impact, measured
through the storage-smoke embedded archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Reuse existing prepared primary-key update coverage, including non-unique
  secondary-index updates, unchanged-row updates, fallback duplicate-key paths,
  statement rollback, and index-cache correctness checks.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Direct prepared point updates preserve row effects and secondary-index
  lookup correctness after repeated updates.
- The direct-update path reuses the cached key-change mask for index-preserve
  and changed-entry decisions.
- No new sidecars, public API changes, file-format changes, or dependencies are
  introduced.

## Risks And Unresolved Questions

- The mask is only valid for the accepted direct-update statement shape. It
  must therefore be reset with direct-update state and used only while
  `direct_update_rows()` is inside `update_row()`.
- This does not remove key serialization for changed indexes; it only avoids
  repeated metadata checks and impossible-key comparisons.
