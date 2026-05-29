# Ownerless Safe Serialized Commit

## Problem

Phase 9 already has ownerless redo reservations, completed-write tracking, and
crash faults around redo publication, but the spec still describes group commit
or safe serialized commit as underdefined. MyLite should not claim cross-process
group commit yet. The product claim for this phase is safe serialized commit:
concurrent ownerless commits may serialize behind redo/page-visibility gaps, but
they must not publish pages past unwritten redo and their committed state must
survive `.ckpt` and `.shm` rebuild paths.

## Source Findings

- MariaDB authority remains baseline `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` calls the MyLite ownerless redo
  hooks from `mtr_t::ownerless_redo_enter()`, `mtr_t::ownerless_redo_leave()`,
  and the mini-transaction redo reservation/write path.
- `packages/libmylite/src/ownerless_redo_state.cc` owns the directory-backed
  redo segment: atomic reservations, contiguous completed-write tracking,
  page-visible clamping to written redo, checkpoint seeding, and active
  reservation cleanup.
- `test_ownerless_concurrent_transaction_commits()` already starts multiple
  independent-table ownerless transactions, releases their commits together, and
  verifies row totals through ownerless and native exclusive reopen before and
  after forced `.shm` rebuild.

## Scope And Non-Goals

This is a test/docs hardening slice. It does not implement group commit, change
MariaDB redo internals, change public APIs, alter directory layout, or reclaim
retained page-version WAL records after native checkpoint reconciliation.

## Design

- Treat safe serialized commit as the current ownerless product posture.
- Keep group commit documented as an optimization candidate, not as supported
  behavior.
- Extend the concurrent commit race test to assert recovery anchors:
  - page-version WAL has committed records or has been checkpointed,
  - the durable `.ckpt` page-visible LSN advances,
  - rebuilt `.shm` redo-visible state is at least the durable checkpoint after
    forced `.shm` recreation.
- Keep the existing row-total assertions through ownerless and native exclusive
  read/write opens.

## Compatibility Impact

No SQL behavior changes. The compatibility matrix gains stronger evidence that
the covered concurrent commit race persists both logical rows and ownerless
recovery anchors.

## Test Plan

- Run focused `commit-race` selector in an embedded build.
- Run ownerless cross-process SQL CTest in an embedded build.
- Run formatting and diff checks.

## Acceptance Criteria

- Concurrent ownerless commit race still reaches the deterministic final total.
- Commit race advances durable page-visible checkpoint state.
- Forced `.shm` rebuild seeds redo-visible state from the durable checkpoint.
- Existing ownerless cross-process SQL coverage remains green.
