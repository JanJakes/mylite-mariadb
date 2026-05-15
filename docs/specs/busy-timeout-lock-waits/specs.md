# Busy Timeout Lock Waits

## Problem

MyLite storage currently protects the primary `.mylite` file with advisory
locks, but conflicts fail immediately with `MYLITE_STORAGE_BUSY`. That is a
correctness gate, not an application-friendly wait policy. The public
`mylite_open_config` already contains `busy_timeout_ms`, and the C API docs
already name `mylite_busy_timeout()`, but neither is wired to storage lock
acquisition.

This slice adds bounded lock waits for MyLite storage operations without
claiming full multi-writer concurrency or SQL transaction lock integration.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc` maps `HA_ERR_LOCK_WAIT_TIMEOUT` to
  `ER_LOCK_WAIT_TIMEOUT`, and MyLite already maps `MYLITE_STORAGE_BUSY` to
  `HA_ERR_LOCK_WAIT_TIMEOUT`.
- `mariadb/sql/handler.cc:handler::ha_external_lock()` wraps engine
  `external_lock()` calls in MariaDB's external-lock wait instrumentation, but
  the engine callback still decides whether a storage-file lock conflict can
  wait or must fail.
- `mariadb/include/thr_lock.h` and `mariadb/sql/handler.cc` pass server
  lock-wait timeouts through MariaDB table-lock paths. MyLite's primary-file
  advisory locks live below that layer in `packages/mylite-storage`, so they
  need their own bounded wait setting.
- `mariadb/storage/innobase/handler/ha_innodb.cc:thd_lock_wait_timeout()`
  shows the InnoDB pattern of reading a connection-level timeout for engine
  lock waits. MyLite's public API should expose a MyLite-owned timeout instead
  of forwarding raw MariaDB server variables.
- `packages/mylite-storage/src/storage.c:lock_file()` currently calls
  `flock(..., LOCK_NB)` once and returns `MYLITE_STORAGE_BUSY` on
  `EWOULDBLOCK` or `EAGAIN`.

## Design

Add a thread-local busy timeout to `packages/mylite-storage`:

- `mylite_storage_set_busy_timeout(milliseconds)` sets the current thread's
  storage lock wait budget.
- `mylite_storage_busy_timeout()` returns the current thread's setting so
  higher layers can restore it after scoped operations.
- `lock_file()` keeps the existing non-blocking `flock()` attempt, but retries
  lock conflicts until the thread-local timeout expires. A zero timeout keeps
  the current immediate-busy behavior.

Add `busy_timeout_ms` storage to each `mylite_db` handle. `mylite_open_v2()`
copies the value from `mylite_open_config` when the caller supplied that field,
and the new public `mylite_busy_timeout(db, milliseconds)` updates the handle.

Wrap file preparation, direct SQL execution, statement preparation, and
prepared statement stepping in a small scope that installs the handle's busy
timeout for the current thread. This lets handler calls made during MariaDB
execution inherit the MyLite timeout without making MariaDB-derived handler
methods depend on `mylite_db`.

## Supported Scope

- Bounded waits for primary-file advisory lock conflicts in storage APIs.
- `mylite_open_v2()` honors `config->busy_timeout_ms` while creating or
  opening a file-backed database.
- `mylite_busy_timeout()` updates the timeout used by later SQL work on that
  handle.
- SQL paths driven through `libmylite` inherit the timeout for MyLite handler
  storage calls made on the same thread.
- Timeout expiry still returns `MYLITE_STORAGE_BUSY`, `MYLITE_BUSY`, or
  MariaDB `ER_LOCK_WAIT_TIMEOUT` through the existing mappings.

## Non-Goals

- Full concurrent writers.
- Deadlock detection, lock priority, or fairness.
- MariaDB `lock_wait_timeout` or InnoDB variable integration.
- Cross-thread propagation when a future runtime delegates work to worker
  threads.
- Waiting on MariaDB metadata locks, table locks, network server queues, or
  non-MyLite files.

## Compatibility Impact

MyLite moves from immediate busy errors to SQLite-like bounded busy waits for
cooperating primary-file lock conflicts when configured. Default behavior
remains immediate busy, preserving existing tests and callers that expect
non-blocking failures.

## DDL Metadata Routing Impact

DDL metadata routing is unchanged. DDL that touches MyLite storage can now wait
for the primary-file lock through the same scoped timeout as row DML.

## Single-File And Embedded-Lifecycle Impact

No new durable companion files are introduced. Lock waits still use advisory
locks on the primary `.mylite` descriptor and release them through the existing
file close lifecycle.

## Public API And File-Format Impact

The file format does not change. The public C API gains the
`mylite_busy_timeout()` function already documented by
`docs/api/libmylite-c-api.md`, and the existing `busy_timeout_ms` open-config
field becomes active.

## Storage-Engine Routing Impact

All routed engines share the behavior because `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, and omitted/default MyLite tables call the same MyLite storage
layer.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol package changes are included. A future protocol wrapper should
map client-visible busy-timeout configuration onto the same `libmylite` handle
setting.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to a small retry loop and
scoped timeout glue.

## Test And Verification Plan

- Add storage tests proving zero-timeout conflicts still return busy.
- Add storage tests proving a configured timeout waits until a child process
  releases an exclusive file lock.
- Add embedded open tests proving `mylite_open_v2()` honors
  `busy_timeout_ms`.
- Add public API coverage for `mylite_busy_timeout()`.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, the
  locking compatibility group, and whitespace checks.

## Acceptance Criteria

- Existing immediate-busy behavior remains the default.
- Configured storage lock waits succeed when the conflicting process releases
  before timeout expiry.
- Configured storage lock waits return busy when timeout expires first.
- `mylite_open_v2()` applies `busy_timeout_ms` during primary-file
  create/open.
- `mylite_busy_timeout()` applies to later SQL work on the same handle.
- Docs and compatibility tables no longer list busy-timeout behavior as
  planned-only.

## Risks And Unresolved Questions

- The timeout is thread-local in storage, so future worker-thread execution
  must explicitly propagate the setting.
- Polling with short sleeps is simple and portable enough for this first slice,
  but a future lock manager should replace it before claiming full write
  concurrency.
- Very large timeout values are best-effort and may overshoot slightly because
  sleep granularity is OS-dependent.
