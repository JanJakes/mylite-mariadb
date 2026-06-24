# Ownerless Commit-Race Stability

## Problem

`test_ownerless_concurrent_transaction_commits` is a core ownerless write
concurrency gate: four independent ownerless processes update separate InnoDB
file-per-table tables inside explicit transactions and commit at the same
barrier. During performance-slice verification, the CTest shard containing
that case timed out at 300 seconds, and an isolated scratch run of the same
case hit MariaDB's rollback-segment shutdown assertion in
`trx_rseg_t::destroy()`:

```text
InnoDB: Assertion failure ... trx/trx0rseg.cc line 386
InnoDB: Failing assertion: !UT_LIST_GET_LEN(undo_list)
```

Concurrency cannot be called complete while this gate can hang or abort. This
slice stabilizes that case by fixing the ownerless shutdown/recovery condition
or, if the root cause is outside the current ownerless layer, by adding enough
diagnostics to make the failure actionable without hiding it.

During the shard-level verification for this slice, the same barrier class was
also found in `test_two_processes_deadlock_on_innodb_rows`: one child could
hold a dirty post-update page-write lock while the other child was still trying
to reach the ready barrier. That row-deadlock gate belongs in the same
stability slice because it is another deterministic test barrier that was
stronger than the ownerless page-write serialization currently claimed by the
product.

## Source Findings

- MariaDB base: MariaDB 11.8.6 (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`),
  as recorded in `docs/architecture/engineering-standards.md`.
- `mariadb/storage/innobase/trx/trx0rseg.cc::trx_rseg_t::destroy()` destroys
  rollback segments during InnoDB shutdown. MyLite's ownerless fork delta
  frees `TRX_UNDO_TO_PURGE` undo entries before MariaDB's upstream assertion
  that `undo_list` is empty because ownerless commits can suppress local purge
  while durable history remains on disk for peer recovery. A diagnostic run of
  the failing case printed a single leftover `TRX_UNDO_TO_PURGE` descriptor;
  the cleanup was skipped because ownerless hook callbacks had already been
  reset before rollback-segment destruction.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c::
  test_ownerless_concurrent_transaction_commits()` opens four ownerless
  children, starts explicit transactions, updates distinct tables, releases the
  commit barrier, waits for every child, then verifies ownerless/native reopen
  and recovery anchors.
- The same test can manufacture a parent-side deadlock when some workers have
  reached the post-update pipe barrier while other workers are blocked in
  ownerless lock waits. The waiting workers cannot signal ready until a
  barrier holder commits and releases its transaction-scoped ownerless locks.
  Direct tracing also showed that parallel worker opens can legitimately wait
  on MariaDB/InnoDB byte-range file locks before any explicit transaction has
  started, so the commit-race harness must not treat directory-open
  serialization as part of the commit barrier.
- `run_ownerless_table_pair_deadlock()` used dirty `UPDATE` statements before
  its ready barrier to make two children own opposite rows before the deadlock
  release. Those dirty updates can also publish transaction-scoped page-write
  locks, so one child may block before it can signal ready. `SELECT ... FOR
  UPDATE` owns the row lock needed for deterministic deadlock setup without
  dirtying the table page before the barrier.
- The row-deadlock case also exposed a mixed-page handoff: a surviving
  transaction could refresh a waited page from raw latest redo and observe a
  peer's uncommitted first-row update instead of the rollback-visible boundary.
  Rollback must publish the current post-undo page image, and waited record or
  page-write refresh must use the page-visible LSN rather than raw latest redo.
- Repeated commit-race runs later exposed a forced-`.shm` rebuild gap after all
  writers had exited: close-time no-live reclaim could treat an initialized
  but payload-empty ownerless WAL as retained because the file-size check used
  only the recovery-header size, then could also truncate committed
  peer-explicit DML page-version WAL while a DML-specific native checkpoint
  marker was still the only durable proof that post-checkpoint file-operation
  redo had been seen. A later ownerless opener rebuilding shared memory then
  lacked page-version evidence for the committed DML pages.
- Ordinary native opens over retained ownerless WAL must use that WAL only as a
  startup recovery/read boundary. If the same ordinary handle later executes a
  native write or locking read, the startup page-version visibility must be
  retired first so stale retained page images cannot shadow the ordinary native
  write.
- `packages/libmylite/src/database.cc` owns ownerless runtime close,
  transaction registry cleanup, final no-live native checkpoint/reclaim, page
  visibility release, and InnoDB hook teardown. The fix should stay in this
  layer unless direct MariaDB evidence shows the fork delta in
  `trx0rseg.cc` is wrong.

## Design

- Track whether ownerless InnoDB hooks were installed during the current
  embedded InnoDB lifetime with
  `mylite_ownerless_innodb_lock_hooks_ever_enabled`. This is separate from
  `mylite_ownerless_innodb_lock_hooks_enabled`, which correctly becomes false
  when callback pointers are reset during MyLite shutdown. Reset the lifetime
  flag at `srv_start()` to the current pre-start hook state and set it on
  ownerless hook installation, so ownerless startup/recovery keeps the
  ownerless shutdown cleanup while ordinary later embedded lifetimes keep
  MariaDB's original rollback-segment assertions.
- In `trx_rseg_t::destroy()`, keep MariaDB's original assertion for ordinary
  lifetimes, but run the existing ownerless `TRX_UNDO_TO_PURGE` descriptor
  cleanup when ownerless hooks were ever enabled. Do not free active, prepared,
  or cached undo descriptors in this path.
- Split the commit-race harness into open/start, update, and commit phases.
  All workers first open the directory and start an explicit transaction, so
  inherited InnoDB file-open byte locks are not counted as commit-race
  failures. After the update phase starts, the parent releases the commit
  barrier when every missing post-update worker is accounted for by the
  combined ownerless InnoDB-lock and page-write waiting counters. Release pipes
  remain open for workers that have not yet signalled post-update readiness,
  so those workers can complete their update, consume the already written
  commit-release token, and commit. The test still checks committed deltas,
  native/ownerless reopen, forced `.shm` rebuild recovery anchors, and now
  asserts both ownerless InnoDB and page-write waiters drain after worker exit.
- Update the table-pair deadlock helper to acquire the first row with
  `SELECT ... FOR UPDATE`, signal readiness, then perform both updates after
  releasing both children together. A MariaDB 1213 deadlock or a 1205 lock
  wait timeout on either post-release update is accepted as the victim path,
  because ownerless scheduling can time out the same wait cycle without
  weakening the one-winner/one-victim invariant. The 1213 path verifies MyLite's
  internal deadlock rollback has already discarded process-local file-op redo;
  the 1205 path verifies the same discard after the test issues explicit
  `ROLLBACK`, because MariaDB leaves the transaction open on lock wait timeout.
- Publish rollback handoff boundaries before ownerless transaction page-write
  release. Rollback publication ignores captured pre-rollback transaction page
  images and publishes the current post-undo buffer page image, flushes the
  transaction's page-write pages to the rollback LSN, and force-publishes that
  LSN as page-visible for local SQL rollback/deadlock handoff even when peer
  explicit transactions are still live. Recovered rollback during ownerless
  native startup does not publish ownerless rollback visibility, because that
  recovered transaction can still belong to a live peer process.
- Keep raw latest redo separate from page-visible handoff refresh. Ownerless
  record-wait, current-read, and forced page-write refresh observe the
  published visible LSN before overlaying a waited page, preventing a
  surviving transaction or startup reader from importing uncommitted peer page
  images during deadlock/rollback handoff.
- Startup buffer-pool page imports use the visible-boundary refresh path and
  bypass the ownerless page-write protocol for the local refresh MTR. The
  refresh imports peer-visible pages into the local buffer pool without joining
  the page-write durable-lock protocol that guards application writes.
- Empty ownerless page-log detection now compares against the initialized
  recovery plus page-log header size. A 192-byte initialized WAL is empty, not
  retained page-version evidence.
- If a DML-specific native checkpoint marker is pending with retained
  page-version records, while no dictionary file-op or autoincrement marker is
  pending, no-live reclaim drains the marker for autocommit and single-owner
  explicit writers whose local close can prove the checkpoint boundary. For
  peer-explicit or no-local closers it leaves the marker and WAL in place. This
  is conservative: it preserves forced-`.shm` rebuild evidence until broader
  native redo/checkpoint reconciliation can prove that native files alone are
  authoritative for that DML boundary, without pinning the common local-writer
  path.
- Ordinary native text and prepared statements retire startup page-version
  visibility before write-class statements or locking reads. Ordinary reads can
  still consume retained ownerless WAL for startup recovery, but a later native
  write observes and updates native current pages instead of reading back the
  retained ownerless snapshot image.

## Scope

- Reproduce and classify the commit-race failure on the current
  `ownerless-concurrency` branch.
- Add focused diagnostics or counters if the current failure mode cannot be
  located from existing output.
- Implement the narrow ownerless hook-lifetime and test-barrier fix needed for
  stable concurrent explicit-transaction commit shutdown/reopen.
- Keep the existing commit-race SQL semantics: each table must contain exactly
  the committed worker delta after ownerless reopen, ordinary reopen, forced
  `.shm` rebuild, and recovery-anchor checks.
- Preserve ordinary native write behavior after retained ownerless WAL recovery:
  a later ordinary `UPDATE` must read back its native write, not the retained
  startup page image.

## Non-Goals

- No broad rewrite of ownerless redo/checkpoint reconciliation. DML-only marker
  and WAL retention remains a bounded conservative behavior, not the final
  checkpoint policy.
- No change to MariaDB transaction isolation semantics.
- No weakening, skipping, or quarantine of the commit-race test.
- No attempt to solve unrelated DDL/file-lifecycle crash classes in this
  slice.

## Compatibility Impact

This slice affects ownerless read/write concurrency reliability, especially
explicit transaction commits across processes. It should not change public C
API signatures, SQL syntax support, storage formats, or ordinary non-ownerless
MariaDB behavior.

## Directory And Native Storage Impact

The database directory layout should remain unchanged. Any fix must preserve
native InnoDB files inside the MyLite directory and keep ownerless `.wal`,
`.ckpt`, and `.shm` recovery semantics compatible with existing recovery
tests.

## Test Plan

- Reproduce with:
  - `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case 3`
  - `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test commit-race`
  - `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test deadlock-rows`
  - `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_peer_uncommitted_update_stays_hidden`
- Run the focused case repeatedly after the fix.
- Run the CTest shard containing the case:
  - `ctest --preset php-embedded-prod -R 'libmylite\.ownerless-cross-process-sql\.3$' --output-on-failure`
- Run the shard containing the row-deadlock case:
  - `ctest --preset php-embedded-prod -R 'libmylite\.ownerless-cross-process-sql\.4$' --output-on-failure`
- Run adjacent ownerless transaction, InnoDB hook, and ordinary open/close
  coverage because the fix touches hook lifetime and shutdown cleanup.
- Run `git diff --check` and production build guards.

## Acceptance Criteria

- The focused `commit-race` case passes repeatedly without timeout or InnoDB
  rollback-segment assertion.
- The focused `deadlock-rows` and explicit-DML deadlock marker cases pass
  without parent-side ready-barrier timeout or mixed 305 table totals, and
  still prove exactly one winner and one victim with either MariaDB 1213 or
  1205 as the victim result. The marker case proves 1213 victims are clean
  before explicit test rollback and 1205 victims are clean after explicit
  rollback.
- The focused hidden-uncommitted-update case passes with a reader opened while
  a peer explicit transaction is live, proving ownerless startup recovery does
  not publish the peer's recovered rollback as a visible boundary and that the
  reader sees the old committed value until the writer commits.
- The relevant ownerless SQL shard passes, or any unrelated failure is isolated
  with direct-case evidence.
- Ownerless and ordinary reopen checks still prove the committed deltas and
  recovery anchors.
- DML marker selectors prove autocommit and single-owner explicit markers drain,
  peer-observed explicit marker/WAL evidence stays retained for forced `.shm`
  rebuild, and ordinary native writes after such a startup read back their own
  updates.
- Docs and compatibility notes describe the fixed instability and any remaining
  risk honestly.

## Risks

- The fix deliberately cleans only `TRX_UNDO_TO_PURGE` descriptors. Any active
  or prepared leftover still trips MariaDB's assertion and needs a separate
  transaction-lifecycle investigation.
- Repeated runs are needed because the original timeout/assertion was
  timing-sensitive.
- Retaining peer-explicit or no-local DML-only marker/WAL evidence is a
  correctness tradeoff that can increase retained page-log work until a later
  slice proves native redo/checkpoint reconciliation for these DML boundaries.
