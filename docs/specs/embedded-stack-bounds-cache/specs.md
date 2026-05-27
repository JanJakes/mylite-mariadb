# Embedded Stack Bounds Cache

## Problem

The VPS does not have `sample` or `perf`, so the prepared row-DML hot path was
profiled with `strace`. A short
`prepared-row-only-update-miss-components 1000 20` trace showed 3052
`gettid`, 3052 `/proc/self/maps` opens, 3054 `prlimit64`, and 3052
`sched_getaffinity` calls. A one-iteration `strace -k` path-filtered to
`/proc/self/maps` showed those opens came from `THD::store_globals()` calling
`my_get_stack_bounds()`, which calls `pthread_getattr_np()` on Linux.

That is execution-envelope overhead around every embedded command. It is not
MyLite storage mutation and it is not durable `.mylite` file I/O.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current branch source ref before this slice:
  `9f7c2885aff597854f3cfd7c721313be74151977`.
- `mariadb/libmysqld/lib_sql.cc::emb_advanced_command()` sets
  `thd->thread_stack` to a local frame address and calls
  `THD::store_globals()` for every embedded SQL command and prepared-statement
  execute.
- `mariadb/sql/sql_class.cc::THD::store_globals()` installs the THD in
  thread-local state, refreshes thread ids, and calls
  `my_get_stack_bounds()` unconditionally.
- `mariadb/mysys/my_stack.c::my_get_stack_bounds()` uses
  `pthread_getattr_np()` when available, and glibc's implementation on this VPS
  repeatedly opens and reads `/proc/self/maps`.
- Stack bounds are properties of the current OS thread. They do not change
  between two embedded commands that run the same THD on the same OS thread.

## Design

Cache successful stack-bound discovery on the `THD` and reuse it while the THD
remains on the same `pthread_self()` value and `thread_stack` still equals the
cached stack-start pointer. `THD::store_globals()` still refreshes current THD,
mysys variables, thread ids, `net.thd`, and lock info on every call. Only the
expensive stack-bound discovery is skipped on same-thread re-entry.

The embedded wrapper must not overwrite `thd->thread_stack` with a transient
local fallback address before every same-thread call, because the cached value
now represents the already-discovered stack bound. It still refreshes the
fallback address before calling `store_globals()` when the THD has no cached
stack bounds or has moved to another OS thread.

## Compatibility Impact

No SQL-visible behavior should change. Stack-depth checks still use the same
thread bounds, and THD migration keeps the existing rediscovery path.

## Single-File And Embedded Lifecycle Impact

No `.mylite` format or file-lifecycle change. The slice removes Linux
procfs-based stack-discovery overhead in the embedded command loop and does not
introduce persistent sidecars.

## Public API, File Format, Size, And Dependencies

No public API change, file-format change, dependency change, or intended size
increase beyond two small `THD` cache fields and narrow embedded wrapper code.

## Tests And Verification Plan

- Build the storage-smoke MariaDB embedded archive with static MyLite storage.
- Relink `mylite_perf_baseline`, `mylite_storage_test`, and
  `mylite_embedded_storage_engine_test`.
- Run the required storage and embedded storage-engine ctests.
- Run `prepared-row-only-update-miss-components 1000 100000` before and after
  the patch.
- Run a short `strace` after the patch to confirm `/proc/self/maps` no longer
  scales with prepared execute count.
- Run the prepared insert component benchmark to make sure the broader roadmap
  performance phase still executes.

Current VPS evidence:

- Before this slice,
  `prepared-row-only-update-miss-components 1000 100000` measured the step at
  `244.372 us/op`.
- After this slice, the same phase measured the step at `9.961 us/op` with
  row-only miss checksum `0`, `100000` statement-scope indexed reads, and no
  mutation writes.
- A short after-patch `strace` over
  `prepared-row-only-update-miss-components 1000 20` recorded 2
  `/proc/self/maps` opens, 2 `sched_getaffinity` calls, and 4 `prlimit64`
  calls, down from 3052, 3052, and 3054 respectively before the patch.
- The same after-patch trace still recorded 3052 `gettid` calls because
  `THD::store_globals()` still refreshes `os_thread_id` on every embedded
  command; that is outside this slice.

## Acceptance Criteria

- Prepared row-only update misses still report checksum `0`.
- Storage counters still show statement-scope indexed reads and no mutation
  writes for the no-match phase.
- Same-thread prepared execute loops no longer call `my_get_stack_bounds()` per
  command.
- Other callers that assign `THD::thread_stack` directly still force stack-bound
  rediscovery because the fallback pointer differs from the cached stack start.
- Existing focused storage and embedded storage-engine tests pass.

## Risks

- `THD::store_globals()` also supports worker-thread reuse and thread-pool
  movement. The cache must be keyed by `pthread_self()` and invalidated by
  `reset_stack()` so those paths keep recomputing stack bounds when needed.
- On platforms where `pthread_getattr_np()` is unavailable, the fallback
  address must still be refreshed before a moved embedded THD calls
  `store_globals()`.
