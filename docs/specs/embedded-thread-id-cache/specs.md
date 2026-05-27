# Embedded Thread Id Cache

## Problem

After the embedded stack-bounds cache removed the repeated
`pthread_getattr_np()` procfs path, a current VPS `strace -c` over
`prepared-row-only-update-miss-components 1000 10000` showed the remaining
traced syscall hotspot is `THD::store_globals()` refreshing `os_thread_id`:

- `gettid`: 13032 calls, 85.18% of traced syscall time.
- `openat`: 139 calls.
- `read`: 39 calls.
- `sched_getaffinity`: 2 calls.
- `prlimit64`: 4 calls.

`os_thread_id` is also a property of the current OS thread. In the embedded
same-thread command loop it does not need to be fetched again for every
prepared execute.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current branch source ref before this slice:
  `e8209a009ffb05993cb8efd9106e61b011b78254`.
- `mariadb/sql/sql_class.cc::THD::store_globals()` installs the current THD
  into thread-local state on every embedded command.
- On Linux, `THD::store_globals()` unconditionally calls
  `syscall(__NR_gettid)` to refresh `os_thread_id`, then records
  `pthread_self()` in `real_id`.
- The previous stack-bounds cache already computes `pthread_self()` and uses it
  as the safe same-thread key for retaining cached stack bounds.

## Design

Compute `pthread_self()` before the Linux `gettid` refresh and reuse
`os_thread_id` when the THD already has cached stack bounds and the current
`pthread_self()` matches the previous `real_id`.

When the THD has no cache yet, when `reset_stack()` invalidated the cache, or
when the THD moved to another OS thread, `THD::store_globals()` still refreshes
`os_thread_id` through `syscall(__NR_gettid)` before installing the new
`real_id`.

This deliberately keeps the cache tied to the existing same-thread proof used
by stack-bound reuse rather than introducing a separate lifetime.

## Compatibility Impact

No SQL-visible behavior should change. The cached value remains valid for
same-thread execution, and thread migration keeps the existing refresh path.

## Single-File And Embedded Lifecycle Impact

No `.mylite` format or file-lifecycle change. The slice only reduces embedded
runtime syscall overhead.

## Public API, File Format, Size, And Dependencies

No public API change, file-format change, dependency change, or THD layout
change beyond the previous slice.

## Tests And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Relink `mylite_perf_baseline` and `mylite_embedded_storage_engine_test`.
- Run the embedded storage-engine test.
- Run `prepared-row-only-update-miss-components 1000 100000` before and after
  the patch.
- Run a short after-patch `strace` to confirm `gettid` no longer scales with
  prepared execute count.

Current VPS evidence:

- Before this slice, after the stack-bounds cache,
  `prepared-row-only-update-miss-components 1000 100000` measured the step at
  `9.961 us/op`.
- After this slice, the same phase measured the step at `8.080 us/op` with
  row-only miss checksum `0`, `100000` statement-scope indexed reads, and no
  mutation writes.
- A `strace -c` over
  `prepared-row-only-update-miss-components 1000 10000` dropped `gettid` from
  13032 calls before this slice to 2 calls after it.

## Acceptance Criteria

- Prepared row-only update misses still report checksum `0`.
- Storage counters still show statement-scope indexed reads and no mutation
  writes for the no-match phase.
- Same-thread embedded command loops no longer call `gettid` for each
  `THD::store_globals()` re-entry.
- A moved THD still refreshes `os_thread_id` before recording the new
  `real_id`.

## Risks

- `os_thread_id` is diagnostic/accounting state used outside MyLite. The cache
  must not hide THD movement across worker threads.
- Coupling the `gettid` cache to the stack-bounds cache means `reset_stack()`
  also forces `os_thread_id` refresh. That is conservative and acceptable.
