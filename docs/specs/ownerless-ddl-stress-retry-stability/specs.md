# Ownerless DDL Stress Retry Stability

## Problem Statement

The ownerless DDL stress statement-lock retry selector is meant to prove that a
pre-execution MyLite ownerless statement-lock `MYLITE_BUSY` is retryable. Local
looping showed three instability modes:

- concurrent child startup could fail inside MariaDB embedded initialization
  before workers reached the deliberate statement-lock contention point;
- the failing InnoDB startup was a real rollback-history boundary issue: an
  empty ownerless WAL and matching native redo header could still leave the
  rollback-segment history tail without enough native page proof for
  `trx_lists_init_at_db_start()`;
- after the forced first statement-lock miss, sessions kept a one-second lock
  wait and could spend the rest of the DDL/DML stress run in harness retry
  loops under load.
- forcing a final-close native purge drain hid native-boundary lag behind
  shutdown latency and made no-live native checkpoint cutover coverage hit the
  90-second watchdog instead of proving a bounded close path.

These failures weaken the stress evidence without changing the product
semantics being exercised.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/srv/srv0start.cc:1304` starts InnoDB and
  initializes process-global native runtime state. MyLite ownerless opens
  serialize this startup, but partial startup failure still leaves native
  cleanup and redo-prefix restore work before the next attempt.
- `mariadb/storage/innobase/srv/srv0start.cc:1350` documents that embedded
  InnoDB restart inside one process is not a trivial upstream shape. MyLite's
  retry path must remain bounded and must run only after cleanup.
- `packages/libmylite/src/database.cc:5124` takes
  `concurrency/mylite-runtime-startup.lock` before ownerless native startup.
- `packages/libmylite/src/database.cc:5151` retries ownerless and native-redo
  repair startup only after `start_runtime()` returns `MYLITE_ERROR`.
- `packages/libmylite/src/database.cc:28872` calls `mysql_server_init()`;
  `packages/libmylite/src/database.cc:28890` handles failure by ending partial
  MariaDB startup state, restoring the saved redo startup prefix, releasing the
  bootstrap/startup locks, and clearing runtime state before retry.
- `mariadb/storage/innobase/buf/buf0flu.cc:2057` and `:2145` skip InnoDB
  checkpoint writes while MyLite ownerless checkpoint suppression is active.
  A no-live ownerless startup whose current native redo header is already
  authoritative must not suppress this native startup recovery/checkpoint work.
- `mariadb/storage/innobase/trx/trx0trx.cc:1955` updates the rollback segment
  history list and undo header state in `trx_t::write_serialisation_history()`.
  MariaDB later restores those lists during InnoDB startup through
  `trx_lists_init_at_db_start()` and rollback-segment restore logic.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:3276` can publish the rollback
  segment plus undo history-proof pair before per-page publication. That fast
  pair must carry enough payload evidence for MyLite startup and reclaim to
  prove the native boundary after DDL/open storms.
- `packages/libmylite/src/database.cc:13938` collects native checkpoint proof
  candidates before ownerless reclaim; startup-critical rollback/undo native
  support pages cannot be discarded merely because they are native-support
  records.
- `packages/libmylite/src/database.cc:reclaim_ownerless_page_log_after_native_checkpoint()`
  is the no-live proof gate for page-version WAL truncation. It must keep WAL
  when records remain above the durable visible boundary or when native
  file-operation, DML checkpoint, autoincrement, or rollback-history evidence
  still needs native proof.
- `packages/libmylite/src/database.cc:collect_ownerless_native_page_checkpoint_record()`
  must include startup-critical system-space page images in space `0`; skipping
  all system-space records would prevent exact proof for native dictionary
  pages that define a safe no-live boundary.
- `packages/libmylite/src/database.cc:release_runtime()` can synthesize native
  rollback-history and dictionary boundary records before shutdown, but it
  must not wait on purge just to force WAL deletion. Retained WAL with a
  native checkpoint obligation is the fast, correct close outcome.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c:11199` makes
  the focused DDL stress retry selector hold the dictionary statement-lock byte
  while children are released.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c:79540` relaxes
  a worker session after the deliberate first `MYLITE_BUSY` miss.

## Design

Raise the ownerless/native-redo startup failure retry budget from eight attempts
to sixty attempts with the existing 100 ms delay. Successful opens are
unchanged; only the cleanup-and-retry path after a failed MariaDB embedded
startup is lengthened to tolerate concurrent startup storms and slow native
recovery handoff.

Disable checkpoint suppression during `mysql_server_init()` when ownerless
startup has already proved that the current native redo header is
authoritative: no live peer, no page-version WAL payload, and no native
file-operation checkpoint marker. After native startup and any no-live reclaim
finish, enable checkpoint suppression before the handle can execute ownerless
writes. Live-peer or retained-WAL startup continues to suppress checkpoints
during startup because current native redo is not the authoritative boundary.

Restore payload-bearing native-support WAL for the normal rollback-history
proof-pair path. The proof-only record format remains available for primitive
coverage, but production history-proof publication now writes the
rollback-segment and undo page images with real page-log checksums on both the
paired fast path and the per-page fallback path. Reclaim treats payload-bearing
rollback/undo, `FIL_PAGE_TYPE_SYS`, and `FIL_PAGE_TYPE_TRX_SYS` native-support
records as startup-critical proof obligations; it must force native flush
through the record page LSN and verify exact native page proof before truncating
the WAL. Proof-only metadata has no exact page bytes, so it fails closed rather
than proving truncation. With a live peer, the same obligations make reclaim
refuse truncation.

Keep final close bounded. No-live close publishes visible-LSN native
rollback-history and system-space dictionary boundary records when it can, and
then lets the normal reclaim/proof path decide whether WAL can be truncated.
It does not call the purge-settle hook during final close. If native
file-operation, DML checkpoint, autoincrement, or rollback-history obligations
remain, the page-version WAL stays durable and the next ownerless or ordinary
native open must recover through that retained evidence. The no-live cutover
tests therefore accept both the fully proven native-only path that can remove
`.wal`/`.shm` and the retained-WAL path that removes only `.shm` and verifies
ordinary reopen without hiding work in shutdown.

Keep the DDL-stress statement-lock retry selector's first statement
nonblocking. After that first forced miss, restore the affected session to the
normal 30-second statement-lock wait used by the harness default instead of a
one-second wait. The selector still proves that `MYLITE_BUSY` can be retried,
but the remaining DDL/DML workload no longer depends on many outer harness
retries under ordinary contention.

## Compatibility Impact

This does not change SQL behavior, MySQL/MariaDB compatibility, storage
formats, public API, or directory layout. The WAL format is unchanged, but the
normal rollback-history pair no longer uses the proof-only encoding; it stores
full native-support page records so startup and reclaim can validate them, and
the per-page history-proof path does the same. The product-visible change is
that ownerless/native-redo opens that hit a transient MariaDB embedded startup
failure can keep retrying for about six seconds after cleanup instead of less
than one second. A clean no-live ownerless startup now also lets native InnoDB
complete ordinary startup checkpoint work before ownerless checkpoint
suppression is enabled for subsequent ownerless writes. Final ownerless close
does not force native purge to make WAL disappear; retained WAL remains a valid
native checkpoint obligation when exact native proof is not yet available.

## Native Storage And Directory Lifecycle Impact

The startup retry still restores the 12 KiB redo startup prefix and recorded
redo file size before each subsequent attempt. The retry budget does not add
new durable files or bypass the ownerless runtime startup lock. It does alter
the production rollback-history proof representation from proof-only metadata
back to payload-bearing native-support records inside the existing page-version
WAL, preserving the ability to retain and replay those records when native
startup proof is not strong enough to truncate them.
Visible-LSN boundary synthesis can append rollback-history and system-space
dictionary records into the existing page-version WAL. Those records remain
inside `concurrency/mylite-concurrency.wal` and are reclaimed only after exact
native proof succeeds.

## Test And Verification Plan

- Hook-build `ownerless-runtime-startup-retry-budget` forces twelve
  total post-`mysql_server_init()` failures, four each across ownerless open,
  ordinary native reopen after ownerless activity, and forced-`.shm` ownerless
  reopen.
- Focused history/native-support selectors cover the normal pair path,
  conservative publish-failure fallback, native-support WAL elision, and mixed
  history-proof checkpoint cutover after restoring payload-bearing pair records.
- No-live native cutover and mixed history-proof cutover selectors cover both
  fully checkpointed native-only reopen and retained-WAL native-obligation
  reopen without a close-time purge drain.
- Random transaction rollback handoff asserts final WAL is either
  checkpointed or retained for a concrete native file-operation, DML
  checkpoint, or rollback-history obligation.
- Ownerless-stress DDL retry selector loops locally to cover the previously
  observed concurrent startup and DML starvation failures, including the
  ordinary-native-initialize to ownerless-open handoff.
- Regular ownerless DDL stress still covers the eight-round DDL/DML workload
  with a one-second statement-lock wait.
- Run format and diff checks before committing.

## Acceptance Criteria

- The focused startup retry hook consumes all forced failures and preserves
  table contents across ownerless and native reopens.
- History-proof selectors continue to prove zero native history flush fallback
  on the normal path, fallback when native-support publication is forced to
  fail, and ordinary native reopen after no-live checkpoint drain or retained
  native-obligation recovery.
- No ownerless close path uses a final purge-settle wait to satisfy those
  assertions.
- The focused statement-lock retry selector passes repeatedly without InnoDB
  startup failures or DML retry exhaustion.
- The regular ownerless DDL stress selector still passes.
- Compatibility documentation states the bounded startup retry and post-miss
  timeout behavior accurately.

## Risks And Non-Goals

- This slice does not claim that every possible native startup failure is
  transient. Real corruption still fails after the bounded retry budget.
- This slice does not expand DDL/file-lifecycle crash recovery coverage.
- This slice does not change product statement-lock retry policy for callers;
  it only stabilizes the stress harness after proving the retryable busy path.
