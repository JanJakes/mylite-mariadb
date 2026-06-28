# Ownerless Mixed Proof Checkpoint Cutover

## Goal

Prove a no-live ownerless checkpoint cutover after a write that publishes both
ordinary user page-version payload and proof-only native-support history
records. After the cutover, deleting the ownerless `.wal` and volatile `.shm`
must still leave ordinary native InnoDB reopen authoritative for the committed
rows.

## Non-Goals

- Changing the page-version WAL format.
- Replacing rollback-segment or undo-header history proof records.
- Broad SYS-page delta enablement.
- External randomized redo/RQG stress.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/buf/buf0flu.cc` implements
  `log_checkpoint_low()` and `log_make_checkpoint()`, which are the native
  InnoDB checkpoint authority for redo durability.
- `mariadb/storage/innobase/log/log0recv.cc` implements
  `recv_recovery_from_checkpoint_start()` and the checkpoint-driven redo scan
  path used by native startup.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  exposes MyLite checkpoint probes through
  `mylite_ownerless_innodb_make_checkpoint()` and
  `mylite_ownerless_innodb_checkpoint_covers_lsn()`.
- `packages/libmylite/src/database.cc` publishes ownerless page-visible LSNs,
  appends proof-only native-support history records, runs no-live reclaim, and
  replays retained page-version WAL before final checkpoint.

## Compatibility Impact

No SQL behavior changes. This strengthens ownerless recovery evidence for
ordinary InnoDB `INSERT ... VALUES` in an explicit transaction by proving that
MyLite can discard ownerless coordination WAL after native checkpoint proof.

## Design

Add one production selector to `mylite_ownerless_cross_process_sql_test`:

- `mixed-history-proof-checkpoint-cutover`

The selector creates an InnoDB table, performs a prepared multi-row explicit
transaction, and asserts:

- the commit used the ownerless visible-fast path,
- user page-version payload was published,
- rollback-segment and undo history-proof native-support records were
  published,
- no conservative native history flush fallback was needed,
- no-live close checkpoints the ownerless WAL,
- ordinary native reopen succeeds after removing both `.wal` and `.shm`.

## File Lifecycle

The slice uses the existing MyLite database directory layout. It intentionally
removes only MyLite coordination files after the no-live checkpoint cutover to
prove the native files in `datadir/` are authoritative.

## Embedded Lifecycle And API

No public API changes. The selector exercises open, explicit transaction,
close-triggered no-live reclaim, forced `.wal`/`.shm` removal, ordinary native
reopen, and final close.

## Build, Size, And Dependencies

No build, binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `embedded-prod`.
- Run focused CTest selector:
  - `ctest --preset embedded-prod -R '^libmylite\.ownerless-mixed-history-proof-checkpoint-cutover$' --output-on-failure`
- Run adjacent production selectors:
  - `explicit-transaction-undo-wal-elision`
  - `no-live-native-checkpoint-cutover-proof`
  - `commit-race`
- Run `ownerless-stress`, format checks, and `git diff --check`.

## Acceptance Criteria

- The focused selector proves both user payload and proof-only history evidence
  were emitted.
- Final no-live close checkpoints the ownerless WAL.
- Removing `.wal` and `.shm` does not lose committed rows.
- Native InnoDB checkpoint coverage reaches the ownerless visible boundary.
- Existing adjacent redo/checkpoint selectors keep passing.

## Risks And Open Questions

- This is deterministic evidence, not randomized redo fuzzing.
- It covers explicit `INSERT ... VALUES`; broader transaction shapes remain
  covered by their existing history-proof selectors and planned randomized
  stress.
