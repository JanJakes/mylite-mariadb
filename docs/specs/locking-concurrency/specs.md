# Locking And Concurrency

## Problem

Rollback-journal recovery protects current publication paths only if one process
owns recovery or mutation at a time. Without an inter-process lock, one writer
can truncate or replace another writer's journal, and readers can observe a
primary file while another process is between journal creation and header
publication.

This slice adds the first storage-level locking gate: process-level advisory
locks on the primary `.mylite` file. It prevents unsafe concurrent writers and
allows multiple readers over a stable committed state. It does not yet implement
multi-writer concurrency, lock waiting, deadlock detection, or SQL transaction
lock integration.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::ha_external_lock()` wraps engine
  `external_lock()` calls and tracks `F_UNLCK` transitions around SQL table
  locks.
- `mariadb/sql/handler.cc:trans_register_ha()` documents that transactional
  engines usually register themselves from `external_lock()`. MyLite still
  advertises `HA_NO_TRANSACTIONS`, so this slice should not register SQL
  transactions yet.
- `mariadb/storage/mylite/ha_mylite.cc:external_lock()` currently returns
  success without storage-file locking; table-level `thr_lock` is still present
  through `store_lock()`.
- `mariadb/storage/myisam/mi_locking.c` and `ha_myisam.cc` show that legacy
  engines map SQL table locks to external file locks, but MyLite needs a
  primary-file lock rather than per-engine sidecar locks.
- `mariadb/include/my_base.h` defines `HA_ERR_LOCK_WAIT_TIMEOUT`, which is the
  closest handler error for a non-blocking storage lock conflict.

## Design

Add advisory locks in `packages/mylite-storage` on the primary file descriptor:

- read APIs acquire a shared non-blocking lock after pending recovery is handled,
- write APIs acquire an exclusive non-blocking lock before pending recovery,
- pending journal recovery acquires an exclusive non-blocking lock before
  restoring header/catalog pages,
- clean file creation uses exclusive create to avoid two creators publishing the
  same primary path.

Locks are held for the lifetime of the storage API's `FILE *` and released by
`fclose()`. A conflict returns `MYLITE_STORAGE_BUSY`. The MyLite handler maps
that to `HA_ERR_LOCK_WAIT_TIMEOUT`.

This is a correctness gate, not the final concurrency architecture. It rejects
conflicting writers instead of waiting or interleaving writes. Later slices can
replace the coarse lock with a lock manager that supports multiple writers,
transactional write sets, and conflict resolution.

Follow-up status: the `busy-timeout-lock-waits` slice adds bounded waits before
returning busy while keeping the same coarse lock ownership model.

## Supported Scope

- Multiple concurrent read opens over the same primary file.
- Rejection of reads while a writer or recovery owns the exclusive lock.
- Rejection of writes while any reader, writer, or recovery owns the file.
- Exclusive recovery of pending rollback journals before reads or writes.
- Handler error mapping for storage lock conflicts.

## Non-Goals

- Cross-process write concurrency.
- Deadlock detection, lock priority, or full multi-writer waiting semantics.
- SQL transaction registration, row locks, gap locks, metadata locks, or
  savepoint-aware lock release.
- Network server connection scheduling or global daemon locks.
- Locking temporary spill or future shared-memory companions.

## Compatibility Impact

Multiple readers move from planned to partial for storage API operations.
Concurrent writers remain planned because this slice rejects conflicting writes
instead of allowing safe multi-writer progress. Cross-process unsafe writers move
from out of scope to explicitly rejected while the lock is held.

## File-Lifecycle Impact

No durable lock companion is introduced. The lock is an OS advisory lock on the
primary `.mylite` file descriptor and disappears when the descriptor closes or
the process exits.

## Test Plan

- Add storage tests that fork a child process holding:
  - an exclusive lock, then assert read and write APIs return busy;
  - a shared lock, then assert readers proceed and writers return busy.
- Add a recovery test where a pending journal exists while another process holds
  a shared lock, proving recovery returns busy until the lock is released.
- Keep rollback-journal cleanup and corrupt-journal tests passing.
- Extend handler error mapping so SQL paths receive a lock-wait style handler
  error for storage busy.
- Run dev, embedded, storage-smoke, tidy, format, diff, and archive-size checks.

## Acceptance Criteria

- Storage operations never recover or mutate the primary file unless they hold
  the exclusive file lock.
- Storage readers hold a shared file lock while reading committed state.
- Lock conflicts return a specific busy result instead of generic I/O or
  corruption errors.
- Clean operation tests prove MyLite still leaves no durable lock sidecar.
- Docs and compatibility tables describe the partial locking guarantee and the
  remaining concurrent-writer work.

## Risks

- Zero-timeout conflicts surface as immediate errors. The follow-up
  busy-timeout API adds bounded waits, but still does not provide full
  multi-writer progress.
- Advisory locks only protect cooperating MyLite processes. Non-MyLite writers
  can still corrupt the file and remain out of scope.
- Coarse file locks are deliberately conservative. They should be replaced by a
  finer lock manager before claiming full write concurrency.
