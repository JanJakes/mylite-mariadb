# Ownerless Random Transaction Rollback Handoff

## Problem

Direct production `random-tx-stress` failed deterministically with
`MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=1`, and later reproduced at round
`3`. Ownerless and ordinary native reopen agreed on persisted rows, but the
totals included updates from workers that ended with full `ROLLBACK` or from
an earlier retry attempt.

That makes explicit rollback/savepoint stress a completion blocker. The
existing focused savepoint selector still passes, so the problem is narrower
than MariaDB savepoint semantics: ownerless transaction page handoff is
publishing or flushing rollback/retry page state as if it were committed.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_random_transaction_stress()` starts four ownerless writer
  processes plus a reader, uses savepoints, full rollback, bounded retry after
  `1205`/`1213`, and validates ownerless, native, and forced-`.shm` reopen
  totals.
- The one-round failure showed row deltas from the round-1 full-rollback
  worker persisted for phase 0 and phase 2, while a concurrent committed
  worker kept a duplicate phase-0 delta after retry. A later round-3 failure
  showed another full-rollback worker leaking a phase-2 delta. That is
  page-version handoff evidence, not an expected-total arithmetic problem.
- `mariadb/storage/innobase/trx/trx0roll.cc:64` runs rollback finish through
  `trx_t::commit()` with `trx->in_rollback` set for ownerless non-read-only
  transactions.
- `mariadb/storage/innobase/trx/trx0trx.cc:2458` publishes transaction pages,
  flushes transaction page-write pages, and force-publishes page-visible LSN
  for full rollback.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:1850`
  skips captured transaction page images during rollback, but still scans and
  publishes current buffer-pool pages for every tracked page.
- `packages/libmylite/src/database.cc` releases ownerless page-write
  transaction ids after successful transaction-ending SQL and discards
  file-operation redo after rolled-back local writes. The missing pieces were
  that rollback should not export user page images as a committed visibility
  boundary, and same-process readers must not keep a stale ownerless overlay
  after rollback.

## Design

Full rollback does not introduce a new committed user-table state. It releases
native row locks and ownerless page-write locks, and leaves the previously
committed visible boundary as the state peers may use.

The rollback branch in `trx_t::commit_in_memory()` will therefore stop
publishing transaction-deferred user page images, stop flushing transaction
page-write pages, and stop force-advancing the ownerless page-visible LSN.
Before releasing transaction page-write locks, rollback refreshes only the
transaction's tracked page-write pages from native storage with external
page-version visibility disabled. SQL-layer transaction-ending rollback with
local writes also closes the current read view, clears page-version visibility
and observations, evicts clean external pages, and installs a per-handle native
read fence until a later successful local write/commit boundary advances the
handle to the latest committed native state.

Explicit SQL transactions retain page-write ownership until transaction end.
Background dirty-page publication skips pages that still have an active
ownerless page-write owner; the committing transaction can still publish its
own active pages when it proves a COMMIT boundary. Transaction page-image
publication validates that the captured image still matches the buffer-pool
page before exporting it.

No-live reclaim must not discard page-version WAL that this runtime consumed
from a peer after the runtime opened if the runtime did not perform the native
write. Such reader-only runtimes retain the newly consumed WAL so a later
startup/rebuild path can materialize it. Runtimes that opened with existing WAL
may reclaim it after startup/rebuild replay proves the native boundary.

Commit keeps the existing fast publication policy, including rollback-segment
and undo history proof pages, but stale transaction publish-failure state no
longer suppresses mandatory history proof page publication. Recovered rollback
during native startup already avoids ownerless rollback visibility and remains
unchanged.

This deliberately prefers conservative peer refresh to an unsafe rollback page
handoff. If a later slice needs a rollback-specific handoff, it must prove a
post-undo page image can be exported without leaking rolled-back row versions
or retry-attempt deltas.

## Compatibility Impact

SQL syntax, public C API, native InnoDB format, and directory layout are
unchanged. The change aligns ownerless full-rollback visibility with
MariaDB/InnoDB semantics: rolled-back DML is not a committed state transition.

## Database Directory And Lifecycle Impact

No files or formats are added. Existing rollback cleanup continues to release
shared ownerless locks and SQL-layer transaction state. Page-version WAL and
native checkpoint files no longer receive a rollback user-page visibility
advance for full rollback. The existing page-version WAL remains durable when
a reader-only runtime consumes peer WAL appended after open and cannot prove
native materialization itself.

## Native Storage Impact

Native InnoDB remains responsible for undo and rollback. Ownerless no longer
flushes transaction-tracked user pages at the rollback boundary, avoiding a
fork-layer durability path that can outpace or contradict native rollback
proofs under concurrent stress. Same-process ownerless overlays are refreshed
from native pages after rollback so later reads cannot reuse a pre-undo page
image.

## Test And Verification Plan

- Add a registered focused production CTest that runs
  `random-tx-stress` with `MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=3`.
- Run the focused new CTest and direct three-round selector.
- Run direct `random-tx-stress` at the default 24 rounds and the ownerless
  stress random transaction CTest where available.
- Run adjacent rollback/savepoint/deadlock/commit-race selectors and explicit
  history-proof selectors.
- Run the production ownerless SQL shard set, ownerless primitives, format
  check, and `git diff --check`.

## Acceptance Criteria

- The three-round random transaction stress no longer leaks full-rollback
  worker deltas or duplicate retry-attempt deltas.
- Default direct random transaction stress passes through ownerless reopen,
  ordinary native reopen, forced-`.shm` ownerless reopen, and final native
  reopen.
- Focused savepoint, deadlock/rollback, commit-race, uncommitted-peer-hidden,
  and explicit transaction history-proof selectors still pass.
- Repeated three-round random transaction stress passes without leaked
  rollback deltas, missing committed deltas, or retained WAL at final close.
- Docs and compatibility notes describe that rollback handoff is conservative
  and does not claim broader redo/checkpoint, DDL/file-lifecycle, or RQG
  completion.

## Risks

- This slice removes an optimistic rollback handoff path. If a peer depends on
  immediate rollback-page publication rather than ordinary lock release plus
  visible-boundary refresh, adjacent commit-race or lock-wait tests should catch
  it.
- Full rollback crash windows, killed sessions with savepoints, broader DDL
  recovery, and long external MariaDB/RQG stress remain separate completion
  items.
