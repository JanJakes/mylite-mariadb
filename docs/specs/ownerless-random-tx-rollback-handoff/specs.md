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

The rollback branch does not publish a restored user page as a new committed
payload or force-advance the ownerless page-visible LSN. After native row undo
finishes, it instead publishes a zero-payload terminal rollback barrier for
each restored user page, flushes the restored native page, and releases that
page's ownerless write ownership. The barrier is ordered at that page's latest
committed page-log boundary. Readers at older snapshot boundaries remain on
the older payload; readers at or after the restored boundary use the native
page instead of replaying the predecessor payload over an already-undone page.
If an attempted commit image has a newer physical page LSN than the restored
post-undo image, the barrier inherits that predecessor ordering LSN and its
later WAL offset wins the tie. A later committed payload for the same page
supersedes the barrier normally.

Rollback-segment and undo support pages use a different terminal rule. Their
earlier crash proof is closed by publishing the terminal native-support image
as ordinary payload, so restart and no-live checkpoint can reclaim the
in-flight proof without losing native rollback-history state.

SQL-layer transaction-ending rollback with local writes closes the current
read view, clears page-version visibility and observations, and evicts clean
external pages. It retains the handle's monotonic committed read LSN: rollback
must not move a handle behind a commit that it observed before the transaction
began. Page-scoped barriers provide native fallback only for restored pages,
while unrelated pages remain eligible for page-version WAL reads.

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

The page index, startup rebuild, stable rebuild, and checkpoint replacement
retain user-page barriers as semantic index entries while excluding
proof-only native-support records. A direct barrier hit still scans the
appended WAL tail before choosing native storage, because a later committed
payload may already supersede a cached barrier. Lifecycle scans classify these
barriers as semantic user-page records so final no-live durability proof and
WAL reclamation cannot strand them as generic proof-only metadata. Native
tablespace replay includes barriers in its per-page winner selection but does
not write them: a winning barrier leaves the already-restored native page
untouched, while a later committed payload is replayed normally.

## Compatibility Impact

SQL syntax, public C API, native InnoDB format, and directory layout are
unchanged. The change aligns ownerless full-rollback visibility with
MariaDB/InnoDB semantics: rolled-back DML is not a committed state transition.

## Database Directory And Lifecycle Impact

No files or record-layout versions are added. The barrier adds a dedicated
metadata bit to the existing record-flags field, alongside the proof-only bit,
and has an empty payload. The dedicated bit prevents an in-flight rollback
crash proof from being interpreted as a completed rollback barrier. Existing
rollback cleanup continues to release shared ownerless locks and SQL-layer
transaction state. Page-version WAL and native checkpoint files do not receive
a rollback user-page visibility advance for full rollback. The existing
page-version WAL remains durable when a reader-only runtime consumes peer WAL
appended after open and cannot prove native materialization itself.

## Native Storage Impact

Native InnoDB remains responsible for undo and rollback. Ownerless flushes the
post-undo native page before releasing its page ownership, but never exports
that restored user page as replayable committed payload. Same-process
ownerless overlays are refreshed after rollback so later reads cannot reuse a
pre-undo image.

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
- A completed rollback does not regress a handle below an earlier committed
  page-version boundary it already observed.
- A terminal user-page barrier survives page-index rebuild and checkpoint
  replacement, is invisible to older snapshots, and yields to later committed
  payload for the same page.
- Native tablespace replay does not reapply an invalidated attempted-commit
  payload when a terminal barrier is the latest visible record for that page.
- Docs and compatibility notes describe that rollback handoff is conservative
  and does not claim broader redo/checkpoint, DDL/file-lifecycle, or RQG
  completion.

## Risks

- Terminal barriers participate in current page selection without carrying a
  replacement payload. Page-index rebuild, checkpoint retention, native-page
  validation, and later-payload supersession must therefore agree on the same
  page-local ordering; primitive and SQL rollback/reopen tests cover those
  boundaries.
- Full rollback crash windows, savepoint rollback crash windows, broader DDL
  recovery, and long external MariaDB/RQG stress remain separate completion
  items. Killed-before-savepoint-rollback recovery is covered by a later
  focused DML marker slice.
