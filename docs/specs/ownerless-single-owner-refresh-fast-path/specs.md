# Ownerless Single-Owner Refresh And Reclaim Fast Path

## Problem

Production timing shows the remaining ownerless performance gap is not PHP
startup. The embedded probe keeps ordinary and ownerless read/startup paths in
the same broad range, while ownerless autocommit writes remain much slower
than ordinary autocommit writes.

A reduced stats-enabled production probe on 2026-06-08 reported, for 80
ownerless autocommit inserts:

- ordinary autocommit inserts: 1740.19 ops/s;
- ownerless autocommit inserts: 145.25 ops/s;
- 1,789 page-write refresh calls;
- 3,892 ownerless page reads;
- 1,268 page-log WAL-scan calls;
- 603 page-log appends.

Increasing the page-write refresh negative-cache capacity from 64 to 1024
entries did not materially reduce misses because the visible LSN advances on
each autocommit.

Further stats showed two larger single-owner costs:

- foreground statement-boundary reclaim could run repeatedly after the 64 KiB
  WAL threshold, accounting for roughly 279 ms of 80 ownerless autocommit
  prepared inserts before throttling;
- pre-statement external refresh for autocommit writes could account for
  roughly 127 ms of the same run after reclaim throttling.

The safe target is the common single-owner case: when no peer process has ever
joined this ownerless runtime, external page refresh work cannot discover
peer-written pages, and foreground reclaim does not need to run on every
thresholded write because the existing timer scheduler also owns idle cleanup.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_enter()` acquires ownerless page-write
  ownership before refreshing a clean page for persistent X/SX writes.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_refresh_page_for_write()` observes shared redo and
  calls `refresh_page_for_write()` to check page-version WAL and native disk
  before a clean page is modified.
- `packages/libmylite/src/ownerless_process_registry.cc` increments the
  process-registry header generation on slot allocation and slot release. The
  allocating process receives the new slot generation.
- Therefore, while the process-registry active count is exactly one and the
  header generation still equals this process slot generation, no other
  ownerless process has joined or exited since this runtime registered.
- Once any peer process opens or a dead peer is cleaned up, the registry
  generation changes and the proof is permanently false for this runtime.
- `packages/libmylite/src/database.cc`
  `refresh_ownerless_external_pages_before_statement()` performs statement
  boundary refresh for ownerless writes before MariaDB execution, and
  `maybe_reclaim_ownerless_page_log_after_statement()` calls
  `reclaim_ownerless_page_log_after_native_checkpoint()` after thresholded
  writes.
- `docs/specs/ownerless-timer-checkpoint-scheduling/specs.md` records the
  runtime-owned timer scheduler that can reclaim checkpointable WAL while the
  runtime is idle. Foreground statement reclaim therefore only needs bounded
  urgency, not per-row repetition.

## Design

Add a shared single-owner epoch proof and use it in two places. The proof is
true only when:

- the process registry is mapped;
- this runtime has a valid owner generation;
- process-registry active count is exactly one;
- process-registry header generation equals this runtime's process-slot
  generation.

An internal ownerless InnoDB hook callback asks MyLite whether external page
refresh can be skipped for a normal page-write refresh. When the callback
returns true, the non-forced page-write refresh returns OK without reading
shared redo, page-version WAL, or native disk. Forced refreshes still use the
existing path because they are used after waits, deadlocks, startup/recovery
edges, or explicit force requests.

The same proof is used before ownerless autocommit write statements that do not
need page-version reads and do not force a native DDL flush. In that case
`refresh_ownerless_external_pages_before_statement()` returns after clearing
local external page visibility because no peer can have published a newer page,
dictionary generation, or tablespace boundary for this runtime to discover.

The no-live-peer reclaim path also uses the proof. After publishing and
flushing this runtime's own dirty pages, a still-single-owner runtime can check
native checkpoint coverage and advance the page-log checkpoint without scanning
external page versions first. Runtimes that have ever seen a peer retain the
existing external refresh before reclaim.

This keeps the fast path narrower than `active_count <= 1`: a process that has
seen a peer join and later exit no longer qualifies, so any stale clean pages
from the peer-written era continue to use conservative refresh.

Foreground statement-boundary reclaim is also throttled. The ownerless runtime
initializes the foreground-attempt timer when the checkpoint scheduler starts,
so newly opened writers defer the first thresholded reclaim to the idle
scheduler for one scheduler interval. Later foreground attempts inside
`MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS` return and leave cleanup to
the timer scheduler or a later statement boundary. Close-time reclaim remains
unthrottled.

Shared-readonly ownerless participants keep a distinct process-registry slot
mode. Page-log reclaim checks for live shared-readonly peers before live
reclaim, so passive ownerless writer peers can still use the existing
statement/timer reclaim path while read-only snapshot participants retain the
WAL until they close.

## Compatibility Impact

No SQL, C API, PHP API, or native storage-format changes. The callback is an
internal MariaDB-hook bridge between MyLite-owned code and the bundled InnoDB
ownerless hook file. Shared-readonly slot mode is an internal process-registry
classification for reclaim policy; public open flags are unchanged.

## Directory And Lifecycle Impact

No durable file or directory-layout changes. The fast paths read existing
process-registry metadata in `mylite-concurrency.shm`. The reclaim throttle is
runtime-local only, and shared-readonly slot mode uses an existing per-slot
open-mode field.

## Native Storage Impact

No native InnoDB format changes. Single-owner refresh skipping is only allowed
before peer participation is possible for the runtime.

## Build And Performance Impact

The combined fast paths reduce single-process ownerless autocommit write
overhead by avoiding redundant external refresh checks and repeated foreground
WAL reclaim. Multi-process ownerless workloads keep the conservative refresh
path after any peer joins. Binary-size impact is limited to one internal
callback, one process-registry generation accessor, runtime-local reclaim
state, and additional perf-probe counters gated behind existing stats flags.

Production probe samples on 2026-06-08:

- baseline before this slice: ownerless autocommit 84.75 ops/s for 200
  inserts, ordinary autocommit 2050.82 ops/s;
- after foreground reclaim throttling and statement refresh skipping:
  ownerless autocommit 159.98 ops/s for 200 inserts, ordinary autocommit
  2034.55 ops/s in the same sample;
- stats-enabled 80-insert sample after both changes: ownerless autocommit
  170.95 ops/s, pre-statement refresh 0.060 ms, single-owner skip allowed
  1,001 of 1,001 callback calls, foreground reclaim 37.114 ms, and page-read
  calls reduced to 937.

## Test Plan

- Build the MariaDB embedded archive and production embedded MyLite targets.
- Run a stats-enabled production performance probe and compare ownerless
  autocommit throughput, prepared-step refresh/reclaim timings, page-read
  counts, and page-write refresh detail counts.
- Run representative ownerless SQL cases:
  `prepared-committed-read`, `native-reclaim`, `live-reclaim`, `commit-race`,
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`, and
  `table-lock-wait-negative-proof`.
- Run CTAS/DDL guard selectors that rejected broader unsafe refresh/page-index
  shortcuts: `ctas-post-create-dml`, `ddl-broader`, and
  `online-ddl-options`.
- Run `ctest --preset prod`.
- Run `cmake --build --preset format-check-prod`.
- Run `cmake --build --preset tidy-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Single-owner ownerless autocommit probe throughput improves or refresh detail
  work materially drops.
- Any peer registry generation change disables the fast path for that runtime.
- Forced refresh paths remain conservative.
- Live shared-readonly peers retain WAL from timer reclaim until they close.
- Focused ownerless correctness selectors pass.
- Production CTest and static checks pass.

## Risks And Follow-Up

- If CI still shows PHPUnit slowdown after this, the next target is likely
  page-log append/publish volume, the remaining foreground reclaim cost, or
  WordPress-specific write mix rather than external page refresh.
- This slice does not solve SQL-level table-lock fault injection or broader
  ownerless DDL/file-lifecycle gaps.
