# Ownerless Live Snapshot Reader-Close Retention

## Problem Statement

The live snapshot-pin reclaim SQL case still expected a repeatable-read reader
that consumed peer page-version WAL to checkpoint that WAL immediately after the
pin released. That expectation predates the rollback-handoff rule that a
reader-only no-live close must not materialize peer WAL appended after the
runtime opened. The stale reader can prove its own snapshot has ended, but it
did not perform the native write and should leave the retained page-version WAL
for a later ownerless startup/rebuild path to materialize and checkpoint.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` checks
  `ownerless_runtime_consumed_page_version_wal`,
  `ownerless_page_log_start_end_offset`, and
  `ownerless_runtime_has_local_write`. With no live peers, if a runtime consumed
  page-version records appended after it opened and did not write locally, it
  returns without checkpointing retained WAL.
- The same reclaim path still permits a later ownerless opener to refresh from
  the retained WAL, prove native checkpoint coverage, and truncate the log on
  close.
- `docs/specs/ownerless-random-tx-rollback-handoff/specs.md` and
  `docs/specs/ownerless-cross-process-concurrency/specs.md` document the
  reader-only no-live close rule as part of conservative rollback and handoff
  recovery.

## Design

Align `test_ownerless_live_snapshot_pin_blocks_page_log_reclaim()` and the
adjacent synthesized-boundary snapshot-pin case with the implemented reclaim
contract:

1. Keep the live reader pin while a peer writer appends page-version WAL.
2. Verify the writer close leaves WAL retained while the pin is active.
3. Release and reap the reader, then require the WAL to remain retained for a
   bounded interval instead of expecting the stale reader close to checkpoint it.
4. Open a fresh ownerless read/write handle, verify the committed aggregate,
   close it, and require the WAL to checkpoint after that fresh startup path.
5. Force `.shm` rebuild and ordinary native reopen to prove the native boundary
   is durable without retained WAL.

No production behavior changes are required for this slice.

## Scope

In scope:

- Focused production and hook SQL coverage for live snapshot reader-close
  retention, including the synthesized native-boundary case.
- Compatibility and cross-process concurrency documentation updates.

Out of scope:

- Page-aware active-pin pruning while a snapshot pin remains live.
- Broader DDL/file-lifecycle recovery.
- Native redo/checkpoint reconciliation beyond this reader-only handoff edge.
- External MariaDB/RQG stress.

## Compatibility Impact

SQL behavior and public API behavior are unchanged. Repeatable-read and
serializable readers still see the snapshot they pinned. The only clarified
contract is lifecycle timing: a stale reader-only process that consumed peer WAL
does not claim responsibility for checkpointing peer-written native state.

## Directory And Lifecycle Impact

No files or formats are added. Retained `concurrency/mylite-concurrency.wal`
records remain inside the MyLite database directory until a fresh ownerless
opener materializes and checkpoints the visible boundary.

## Native Storage Impact

Native InnoDB remains the durability authority after the fresh ownerless opener
has replayed/refreshed the retained page-version boundary and closed through the
existing native checkpoint proof. The stale reader does not use newer native
page LSNs as successor proof for peer-written WAL.

## Test Plan

- Reproduce the old failures with
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_live_snapshot_pin_blocks_page_log_reclaim`
  and
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_live_snapshot_pin_synthesizes_page_boundary`.
- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused production SQL case and the `live-reclaim` selector.
- Build and run the focused hook-build SQL case and adjacent hook selectors
  that cover active-pin reclaim boundaries.
- Run relevant ownerless stress or shard checks if the focused selector exposes
  broader instability.
- Run `format-check-prod`, `git diff --check`, and staged diff checks before
  commit.

## Acceptance Criteria

- The stale reader-close path retains peer WAL after the snapshot pin releases.
- A fresh ownerless opener reads the committed data and checkpoints retained
  WAL on close.
- Forced `.shm` rebuild plus ordinary native reopen still preserve the final
  committed rows without retained WAL.
- Existing live idle-peer, live writer, synthesized-boundary, active-pin
  boundary, and killed-pin reclaim cases continue to pass.

## Risks And Open Questions

- This does not reduce WAL growth for long-running active pins; it only makes
  the post-release reader-only handoff contract explicit.
- Broader redo/checkpoint, DDL/file-lifecycle, transaction crash, pressure
  crash/oracle, and external randomized stress gates remain open completion
  items.
