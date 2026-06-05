# Ownerless Timer Prepared Result Gating

## Problem Statement

The ownerless timer checkpoint scheduler reclaims retained page-version WAL
after a reader pin releases while a writable runtime stays open and idle. The
scheduler is only safe while the process has no active ownerless SQL statement,
prepared result cursor, or explicit transaction. Existing timer coverage proves
the positive idle-writer path, but it does not directly prove that an open
prepared result cursor keeps the timer from forcing native checkpoint
reclamation until that cursor is reset or finalized.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/mysql.h` declares `mysql_thread_init()` and
  `mysql_thread_end()`, which the timer scheduler uses before calling
  MariaDB/InnoDB checkpoint hooks from its background thread.
- `packages/libmylite/src/database.cc` starts the scheduler only for writable
  ownerless runtimes and has the scheduler check
  `ownerless_active_statement_count`, active explicit transactions, checkpoint
  threshold state, and page-version pins before it calls
  `reclaim_ownerless_page_log_after_native_checkpoint()`.
- `packages/libmylite/src/database.cc:mylite_step()` marks ownerless prepared
  statements active while executing. For result statements, it transfers that
  activity to the `mylite_stmt` handle by setting
  `ownerless_runtime_statement_active` before returning rows.
- `packages/libmylite/src/database.cc:release_statement_results()` and
  `clear_statement_ownerless_runtime_activity()` release the runtime activity
  when a prepared result is exhausted, reset, or finalized.

## Design

Extend the focused `timer-checkpoint-scheduling` ownerless SQL coverage with a
second subcase:

1. Create a large InnoDB table and cleanly checkpoint the ownerless WAL.
2. Hold a shared read-only repeatable-read snapshot in a peer process.
3. Commit an ownerless update so the snapshot pin retains page-version WAL.
4. Open a second ownerless handle in the same runtime, execute a prepared
   `SELECT`, fetch one row, and leave the result cursor active.
5. Release the shared read-only snapshot pin and wait long enough for the
   scheduler interval to pass several times.
6. Verify the WAL remains retained while the prepared cursor is active.
7. Finalize the prepared cursor and verify the same open writer runtime
   checkpoints the WAL without another SQL statement.
8. Verify ownerless and ordinary native reopen read the committed final rows
   after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- SQL-level coverage that a live prepared result cursor blocks timer-driven
  ownerless checkpoint reclamation.
- Positive coverage that finalizing the cursor releases the runtime activity
  and allows timer reclamation without another writer statement.
- Documentation and compatibility updates for the timer scheduler proof.

Out of scope:

- New scheduler behavior, public checkpoint APIs, or configuration.
- Reclaim while active snapshot pins remain live.
- Page-aware reader pruning, DDL/file lifecycle redesign, or SQL-level
  table-lock fault injection.
- External MariaDB/RQG pressure stress.

## Compatibility Impact

SQL behavior and public API behavior do not change. The slice strengthens the
performance-safety evidence for ownerless checkpoint scheduling by proving a
prepared result cursor is treated as live same-process statement activity.

## Directory And Lifecycle Impact

No durable file is added. The test observes existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, `.shm`, and native InnoDB table
files under the MyLite database directory.

## Native Storage Impact

Native checkpointing remains delegated to the existing
`mylite_ownerless_innodb_make_checkpoint()` and checkpoint-coverage predicate.
The slice does not alter InnoDB redo, page-version WAL records, or tablespace
formats.

## Public API, Build, Size, And Dependencies

No public API, build profile, binary-size, license, or dependency changes are
intended. The slice adds focused test and documentation coverage.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `timer-checkpoint-scheduling` in `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run adjacent `statement-checkpoint-scheduling`, `live-reclaim`, and
  `active-reader-pressure` selectors.
- Run the `ownerless-stress` active-reader and expanding-page pressure stress
  entries.
- Run the relevant ownerless SQL CTest shard(s), `format-check`, and
  `git diff --check`.

## Acceptance Criteria

- The timer scheduler checkpoints retained WAL after a shared read-only
  snapshot pin releases when the writer runtime is idle.
- The scheduler does not checkpoint retained WAL while a prepared result cursor
  remains active in the same ownerless runtime.
- Finalizing the prepared cursor allows timer checkpointing before the writer
  handle closes and without another writer SQL statement.
- Ownerless and ordinary native reopen read the committed final rows before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- The negative assertion uses a bounded wait. If a future platform cannot run
  the scheduler within that interval, the positive post-finalize wait still
  fails and exposes the scheduling issue.
- Broader page-aware pruning, DDL/file lifecycle recovery, SQL-level
  table-lock fault injection, and external randomized pressure stress remain
  separate ownerless-concurrency gaps.
