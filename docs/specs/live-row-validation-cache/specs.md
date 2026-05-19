# Live-Row Validation Cache

## Problem

After active exact-index cache maintenance, primary-key updates in a transaction
still spent most time validating that the target row id was live. The local
sample showed `mylite_storage_update_row_with_index_entries()` dominated by
`validate_direct_live_row()` and `row_is_hidden_after()`, which rescanned and
checksummed later row-state pages for every update.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::read_index_cursor_row()` and the
  storage index lookup APIs establish row ids that are live for the current
  statement/transaction view.
- `packages/mylite-storage/src/storage.c::validate_direct_live_row()` verifies
  update/delete targets by reading the row page and calling
  `row_is_hidden_after()` unless the storage layer has stronger local evidence.
- SQL updates inside an explicit transaction use the active MyLite storage
  checkpoint, so transient per-checkpoint state can be reused safely until
  commit, rollback, or savepoint rollback.

## Design

- Add a live-row validation cache to active storage checkpoints.
- Mark row ids live when they are returned by exact index lookups, index entry
  reads, visibility-checked row reads, and table scans.
- During update/delete validation, trust a cached live row id after confirming
  the row page still belongs to the target table.
- Maintain the cache across successful mutations by removing the hidden source
  row id and adding the replacement row id.
- Keep non-checkpointed direct storage calls on the existing row-state scan
  path, and clear live-row caches on truncate/catalog invalidation and nested
  checkpoint rollback.

## Compatibility Impact

SQL-visible behavior is unchanged. Cached row ids come from storage reads that
already proved visibility for the active checkpoint. If no cached proof exists,
the old row-state scan remains the source of truth.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. Cached live row ids are
transient memory owned by the active checkpoint and are released on checkpoint
close.

## Test And Verification Plan

- Existing storage lifecycle, update/delete, statement rollback, transaction,
  and savepoint tests cover row-state behavior.
- Rebuild storage and storage-smoke targets.
- Run storage unit tests and the storage-engine compatibility harness.
- Run the local performance baseline to measure primary-key update impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Handler-driven active-checkpoint update/delete validation avoids full repeated
  row-state rescans after the row id was read from the same checkpoint view.
- Existing rollback and savepoint behavior remains correct.
- Existing storage and routed-engine compatibility tests pass.
- Primary-key update timings improve materially in the local benchmark.

## Risks

- Direct C API callers that update arbitrary row ids without first reading them
  still use the scan path. That preserves correctness and keeps the cache scoped
  to rows proven live through the active view.
- `mylite_storage_read_indexed_row(s)` deliberately does not mark rows live on
  its own because those APIs trust the caller's row ids; callers should reach
  them through storage index-entry reads when they need cached validation.
