# Ownerless Statement Checkpoint Scheduling

## Problem

Ownerless page-version WAL reclamation is safe after the native checkpoint
proof added by the native-reclaim slices, but product reclamation has mostly
depended on runtime close. Long-lived ownerless writers can therefore leave
checkpointable page-version WAL records in place even after the existing live
reclaim gates would allow compaction.

The next bounded performance slice is statement-boundary scheduling: after a
successful ownerless write, DDL, or transaction-end statement grows the
page-version WAL past the checkpoint byte threshold and no peer process is
live, MyLite should opportunistically run the already-proven no-live reclaim
path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:2471-2567` executes direct SQL through
  ownerless write/DDL policy checks, statement locks, native MariaDB execution,
  dictionary publication, and transaction-state updates.
- `packages/libmylite/src/database.cc:1675-1800` executes prepared statements
  through the same ownerless policy, locking, dictionary, and transaction
  update flow before returning `MYLITE_DONE` for non-result statements.
- `packages/libmylite/src/database.cc:6568-6663` already owns the safe native
  checkpoint reclamation path, including live-peer statement gating, page-pin
  checks, native write/recovery-idle proof, native checkpoint proof, and page
  index replacement.
- `docs/specs/ownerless-live-checkpoint-writer-gating/specs.md` keeps live
  checkpoint reclamation conservative while another ownerless writer or native
  recovery-sensitive state is active.

## Scope And Non-Goals

In scope:

- Add an internal statement-boundary checkpoint trigger for ownerless direct
  and prepared non-result statements.
- Reuse the existing native checkpoint reclamation path rather than adding a
  separate truncation rule.
- Trigger only after successful write, dictionary DDL, or transaction-end
  statements, after the connection is no longer inside an explicit
  transaction, no peer ownerless process is live, and only when the
  page-version WAL payload is at or above
  `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES`.
- Add SQL coverage proving a long-lived ownerless writer reclaims WAL before
  closing.

Out of scope:

- A timer-driven or thread-backed background worker.
- New public checkpoint APIs or configuration fields.
- Changing the native checkpoint safety predicates.
- External MariaDB/RQG long-running stress.

## Design

After a successful direct SQL statement returns from MariaDB, publishes any
dictionary generation, and updates ownerless transaction state, MyLite releases
the statement byte-range locks and asks whether checkpoint scheduling is due.
Prepared non-result statements follow the same path before returning
`MYLITE_DONE`.

The scheduler is intentionally small:

- skip non-ownerless, read-only, and still-in-transaction handles,
- skip statements that are not writes, dictionary DDL, or transaction ends,
- skip WAL files whose payload is below the checkpoint threshold,
- skip while any peer ownerless process is live,
- skip while any page-version snapshot pin is active,
- call `reclaim_ownerless_page_log_after_native_checkpoint()` when due.

The reclaim function still decides whether compaction is actually safe. The
scheduled path deliberately avoids live-peer reclamation because zero active
page-version pins do not prove other processes have no dirty native state
between statements. Live-peer and active-pin boundary reclamation remain on the
existing close-time path. If any predicate fails, the scheduled attempt leaves
the WAL intact.

## Compatibility Impact

No SQL semantics change. This improves bounded ownerless write performance and
space behavior by making checkpointable page-version WAL records eligible for
reclamation during long-lived single-process runtimes instead of only on close.

The claim remains partial: this is statement-boundary scheduling, not an
independent timer-driven background checkpoint thread. A separate worker would
need thread-local MariaDB/InnoDB lifecycle proof and same-process statement
coordination.

## Database Directory And Lifecycle Impact

No directory layout changes. The scheduler reads the existing
`concurrency/mylite-concurrency.wal` size and reuses existing
`concurrency/mylite-concurrency.ckpt`, `.shm`, and byte-range lock state.

## Native Storage Impact

Native InnoDB checkpointing still runs only through the existing internal hook
and only after the native checkpoint coverage predicate succeeds.

## Public API Impact

No public API changes.

## Binary Size Impact

No new dependency and no public ABI impact. The production change is a small
internal scheduling predicate and two call sites.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `statement-checkpoint-scheduling` in `embedded-dev`.
- Run focused `statement-checkpoint-scheduling` in `ownerless-test-hooks`.
- Run adjacent `live-reclaim`, `native-reclaim`, and pressure selectors.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- A successful ownerless write that grows the page-version WAL past the
  checkpoint threshold reclaims the WAL before the handle closes when no peer
  ownerless process is live.
- Ownerless and ordinary native reopen still read the committed rows after a
  forced `.shm` rebuild.
- Existing live-writer and active-reader reclaim gates continue to block unsafe
  native checkpoint reclamation.

## Risks And Follow-Up

- This is opportunistic and intentionally conservative. If another ownerless
  process is live, an active pin is present, or the existing no-live reclaim
  proof fails, the scheduled attempt leaves WAL retention unchanged.
- A true independent background worker remains future work because it needs
  MariaDB thread lifecycle proof and same-process statement coordination beyond
  POSIX byte-range locks.
