# Ownerless Live-Peer Statement Checkpoint Gating

## Problem Statement

Ownerless statement-boundary checkpoint scheduling is intentionally no-live
only. Close-time live-peer reclaim has stronger lifecycle evidence because it
runs as a handle is closing and uses the ownerless statement gate, pin
snapshot, and native write/recovery-idle proof before forcing a native InnoDB
checkpoint. Running the same optimization between statements while other
processes remain open is not yet proven: peers can be between statements while
still carrying process-local native state from earlier writes.

This slice adds focused negative coverage so future scheduling changes do not
accidentally reclaim page-version WAL at statement boundary while any peer
ownerless process remains live.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  - direct and prepared non-result statements call
    `maybe_reclaim_ownerless_page_log_after_statement()` after successful
    write/DDL/transaction-end statements.
  - that scheduler requires `ownerless_runtime_has_no_live_peers()` before it
    calls `reclaim_ownerless_page_log_after_native_checkpoint()`.
  - close-time `reclaim_ownerless_page_log_after_native_checkpoint()` keeps the
    broader live-peer path gated by the nonblocking ownerless statement gate,
    active pin state, and native write/recovery-idle proof.
- `docs/specs/ownerless-live-checkpoint-writer-gating/specs.md`
  records the native redo risk behind live-peer checkpointing while peer write
  or recovery-sensitive state is active.
- `docs/specs/ownerless-statement-checkpoint-scheduling/specs.md`
  records the first statement scheduler as no-live-only and leaves live-peer
  scheduling to future work.

## Design

Extend the focused `statement-checkpoint-scheduling` SQL selector:

1. Keep existing no-live direct write scheduling coverage.
2. Keep existing explicit transaction `COMMIT` scheduling coverage.
3. Start a live idle ownerless peer, then update a large InnoDB table from a
   second ownerless handle.
4. Verify the page-version WAL remains retained before the writer handle
   closes, proving statement-boundary scheduling did not run while the peer was
   live.
5. Close the writer and verify the existing close-time live idle-peer reclaim
   still checkpoints the WAL.
6. Release the peer and verify ownerless/native reopen still reads the final
   table state after forced `.shm` rebuild.

## Scope

In scope:

- Negative SQL coverage for the current no-live-only statement scheduler gate.
- Documentation that live-peer statement scheduling remains planned and must
  require stronger lifecycle proof.

Out of scope:

- Implementing live-peer statement-boundary checkpoint scheduling.
- Timer-driven or background checkpoint workers.
- Redesigning native InnoDB redo/checkpoint files for process-shared group
  checkpointing.
- New public APIs or configuration.

## Compatibility Impact

SQL behavior is unchanged. The slice strengthens correctness evidence for an
existing partial performance claim: statement-boundary scheduling can reclaim
before close only when no peer process is live. Live idle peers still rely on
the existing close-time reclaim path.

## Directory And Lifecycle Impact

No directory layout changes. The test observes existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, `.shm`, and statement-lock
behavior.

## Native Storage Impact

No storage format changes. The slice deliberately avoids adding new native
checkpoint calls while peers remain live at statement boundaries.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The implementation change is test and
documentation coverage for the existing gate.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `statement-checkpoint-scheduling` in `embedded-dev`.
- Run focused `statement-checkpoint-scheduling` in `ownerless-test-hooks`.
- Run adjacent `live-reclaim` and `native-reclaim` selectors.
- Run ownerless stress or a focused stress selector to ensure the no-live gate
  remains safe under concurrent writers.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- No-live statement-boundary checkpoint scheduling still reclaims WAL before
  close.
- Transaction-end statement scheduling still reclaims WAL after `COMMIT`.
- With one live idle ownerless peer, a successful ownerless write that grows
  WAL past the checkpoint threshold leaves WAL retained before the writer
  closes.
- Closing the writer with the idle peer still allows the existing close-time
  live-peer reclaim path to checkpoint WAL.
- Ownerless and ordinary native reopen still read the committed rows after
  forced `.shm` rebuild.

## Risks And Open Questions

- This does not solve live-peer statement scheduling; it records the current
  safety boundary. Future live-peer scheduling needs stronger proof that open
  peers do not carry process-local native state that can race a forced native
  checkpoint between statements.
- Independent timer-driven checkpointing remains separate work because it needs
  MariaDB thread lifecycle proof and same-process statement coordination.
