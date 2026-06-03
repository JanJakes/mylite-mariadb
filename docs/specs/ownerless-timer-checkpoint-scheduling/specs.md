# Ownerless Timer Checkpoint Scheduling

## Problem

Ownerless page-version WAL reclamation can now run at write, DDL, and
transaction-end statement boundaries, including idle live-peer cases that pass
the existing native reclaim gate. A long-lived writer can still become idle
after the last blocking reader pin releases, leaving checkpointable WAL in the
database directory until another write statement or close-time cleanup runs.

This slice adds a bounded runtime-owned timer scheduler that reuses the proven
native checkpoint reclaim path while the ownerless runtime remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/mysql.h` declares `mysql_thread_init()` and
  `mysql_thread_end()`; `mariadb/libmariadb/man/mysql_thread_init.3` and
  `mysql_thread_end.3` document the thread lifecycle requirement for
  multi-threaded clients.
- `mariadb/storage/innobase/include/log0log.h` declares
  `log_make_checkpoint()`, and
  `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  wraps it in `mylite_ownerless_innodb_make_checkpoint()` after checking that
  InnoDB recovery is not active and the server was started.
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` already owns the safe
  reclaim boundary: checkpoint LSN reads, no-live page-visible advancement,
  live-peer statement gating, page-version pin checks, native write/recovery
  idle checks, native checkpoint proof, and page-index replacement.
- `packages/libmylite/src/database.cc`
  `maybe_reclaim_ownerless_page_log_after_statement()` already schedules the
  same reclaim path after successful ownerless write/DDL/transaction-end
  statements when the WAL crosses
  `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES`.
- POSIX byte-range locks are process-scoped, so a background thread cannot rely
  on the statement-lock file to exclude foreground statements in the same
  process. MyLite therefore needs a runtime-local active statement counter in
  addition to the existing cross-process statement gate.
- A process can also be idle between SQL statements while an explicit
  transaction remains open. The timer and live-peer reclaim gates therefore need
  a runtime-local count mirrored into the ownerless process slot so other
  processes can treat that idle explicit transaction as native write state.

## Scope And Non-Goals

In scope:

- Start one background checkpoint scheduler for a writable ownerless runtime.
- Wake periodically using `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS`
  and run only when the WAL is above the checkpoint threshold.
- Register the scheduler thread with MariaDB using `mysql_thread_init()` and
  release thread-local MariaDB state with `mysql_thread_end()`.
- Skip while this process has any active ownerless SQL statement or prepared
  result cursor, or while this process has any active explicit ownerless
  transaction.
- Reuse the existing native reclaim path and its live-peer/native-idle
  predicates.
- Add SQL coverage proving an idle open writer reclaims WAL after a shared
  read-only snapshot pin releases, without executing another SQL statement or
  closing the writer.

Out of scope:

- New public checkpoint APIs or configuration fields.
- Changing native checkpoint safety predicates or page-version WAL format.
- Reclaim while active pins remain live.
- Dead-owner process-registry cleanup policy changes.
- External MariaDB/RQG long-running oracle execution.

## Design

`RuntimeState` owns a condition variable, scheduler thread, stop flag, and
runtime-local active ownerless statement count. Writable ownerless startup
initializes the normal embedded runtime first, sets `ref_count`, records the
ownerless/read-only mode, then starts the scheduler. If thread creation fails,
the open fails and cleans up the partially initialized embedded runtime.

The scheduler loop:

1. Calls `mysql_thread_init()` before using MariaDB/InnoDB facilities.
2. Sleeps for `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS` or until close
   requests stop.
3. Holds `g_runtime.mutex` while checking runtime lifetime, ownerless mode,
   active same-process statements, active explicit transactions, process-slot
   generation, WAL threshold, and page-version pin state.
4. Calls `reclaim_ownerless_page_log_after_native_checkpoint()` only after
   those predicates pass.
5. Calls `mysql_thread_end()` before exiting.

Direct SQL execution marks a runtime-local ownerless statement active from
after policy/pressure checks through native execution, dictionary publication,
and statement-boundary reclaim. Prepared statements mark runtime activity while
executing; prepared result statements transfer that activity to the statement
handle until result exhaustion, reset, or finalize.

Explicit transaction state is tracked separately. Successful `START
TRANSACTION`/`BEGIN`, `COMMIT`/`ROLLBACK`, implicit autocommit reset, and
close-time rollback update a runtime-local explicit-transaction count and mirror
that count into the current process slot. Live-peer reclaim scans live process
slots and skips native checkpoint reclamation while any slot reports a nonzero
explicit-transaction count.

Shutdown converts the runtime mutex hold to a `std::unique_lock`, stops and
joins the scheduler before native close-time reclaim and before
`mysql_server_end()`, then proceeds through the existing shutdown path.

## Compatibility Impact

SQL results and public API behavior do not change. The implementation improves
ownerless space-pressure behavior for long-lived embedded writer runtimes by
making checkpointable WAL eligible for reclamation after readers release pins,
even when no subsequent SQL statement is executed.

The feature remains partial because it is a bounded scheduler over the existing
reclaim proof, not a new native checkpoint protocol.

## Database Directory And Lifecycle Impact

No durable files are added. The scheduler reads and reclaims the existing
`concurrency/mylite-concurrency.wal` using the existing `.ckpt`, `.shm`, and
statement-lock coordination files. All durable state remains inside the
MyLite-owned database directory.

## Native Storage Impact

Native InnoDB checkpointing still runs only through the existing
`mylite_ownerless_innodb_make_checkpoint()` wrapper and only when
`reclaim_ownerless_page_log_after_native_checkpoint()` can prove the checkpoint
covers the page-visible LSN selected for compaction.

## Public API Impact

No public API or ABI changes.

## Binary Size Impact

No new dependency is added. The runtime adds one `std::thread` and
`std::condition_variable` in embedded ownerless read/write mode.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `timer-checkpoint-scheduling`.
- Run adjacent `statement-checkpoint-scheduling`, `active-reader-pressure`,
  and `live-reclaim` selectors.
- Run `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`.
- Run `ctest --preset ownerless-stress --output-on-failure`.
- Run `format-check` and `git diff --check`.
- Confirm no `/tmp/mylite-ownerless-*` directories or ownerless test processes
  remain.

## Acceptance Criteria

- A writer that remains open and executes no further SQL after a shared
  read-only snapshot pin releases observes page-version WAL checkpointing
  before close.
- Active snapshot pins still retain page-version WAL until release.
- Idle explicit ownerless transactions still retain page-version WAL until they
  end, even when no SQL statement is currently executing.
- Statement-boundary scheduling still reclaims no-live and idle live-peer WAL.
- Ownerless and ordinary native reopen read the committed final rows after a
  forced `.shm` rebuild.

## Risks And Follow-Up

- The timer is intentionally conservative. If same-process SQL, an active
  explicit transaction, live peer native state, active pins, or native
  checkpoint proof block reclaim, WAL remains retained for later
  statement-boundary or close-time cleanup.
- Broader DDL/file lifecycle recovery, SQL-level table-lock fault injection,
  and external randomized oracle execution remain separate ownerless gaps.
