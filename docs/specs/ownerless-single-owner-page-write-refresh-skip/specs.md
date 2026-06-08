# Ownerless Single-Owner Page-Write Refresh Skip

## Problem

Ownerless page-write refresh is expensive in single-process embedded workloads
because it can re-enter page-version lookup and disk-read paths even when no
external ownerless writer can exist. The existing single-owner fast path proved
that these non-forced page-write refreshes are unnecessary while the process
registry still contains only the current owner generation.

The performance probe and focused ownerless SQL tests need sharper evidence for
that fast path so future changes can distinguish:

- still-single-owner page-write refresh skips;
- active-reader snapshots, which must keep the conservative path;
- cold runtimes with no checkpoint baseline yet, which must first establish a
  native read LSN before snapshot pins can be published.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_enter()` calls
  `mylite_ownerless_innodb_refresh_page_for_write()` before modifying clean
  pages when a refresh is required.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_refresh_page_for_write()` asks MyLite's
  `skip_external_page_refresh` callback before non-forced refresh work.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_skip_external_page_refresh_hook()` owns the proof using the
  process registry, page-version pin registry, and redo-visibility state.
- `seed_ownerless_native_checkpoint_baseline()` must return an existing
  checkpoint LSN to `START TRANSACTION WITH CONSISTENT SNAPSHOT`; otherwise a
  checkpointed ownerless WAL can leave the reader with no page-version pin and
  disable active-reader pressure throttling.

## Design

Keep the MariaDB-side fast path scoped to non-forced page-write refresh. Factor
the callback invocation into a local helper so future native call sites do not
duplicate callback/context loads.

The MyLite callback allows the skip only when all of these are true:

- the runtime has one active process;
- the shared process-registry generation still equals this owner's generation;
- the page-version pin registry has zero active pins;
- redo-visibility state has a nonzero latest or visible LSN baseline.

If a checkpoint file already contains latest or visible LSN state,
`seed_ownerless_native_checkpoint_baseline()` now returns that LSN to the
consistent-snapshot caller. This keeps repeatable-read snapshot pins active
across checkpointed ownerless WALs and preserves pressure-limit enforcement.

## Compatibility Impact

No SQL, C API, PHP API, or storage-format changes. The fast path remains
internal to ownerless InnoDB page-write refresh. Active-reader and peer-history
cases keep the conservative refresh path.

## Directory And Lifecycle Impact

No durable file or directory-layout changes. The proof reads existing
`mylite-concurrency.shm` process, page-pin, and redo-visibility segments plus
the existing checkpoint LSN.

## Native Storage Impact

No native InnoDB format changes. Forced refreshes, active snapshot pins, and
missing-baseline opens continue through the native refresh path.

## Build And Performance Impact

The production MariaDB embedded archive must be rebuilt after the native helper
refactor. CI timing jobs should use production build artifacts:

- `php-embedded-prod`/`Release` for MyLite, the PHP extension, and PHPUnit;
- the repository production MariaDB embedded baseline (`MinSizeRel`) for the
  bundled native archive.

Reduced production probe on 2026-06-08, with
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`,
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
`MYLITE_PERF_SELECT_ITERATIONS=100`, and
`MYLITE_PERF_INSERT_ITERATIONS=400`, reported:

- ownerless autocommit inserts: `194.25 ops/s`;
- single-owner skip allowed: `5203` of `5203` callback calls;
- single-owner skip active-pin blocks: `0`;
- single-owner skip baseline blocks: `0`;
- page-write refresh detail calls: `1600`;
- page-version read calls: `1351`;
- page reads through the MyLite page-version hook: `4943`.

The rejected broader buffer-pool/space-header skip eliminated most of the page
reads in this probe but broke active-reader pressure by allowing a checkpointed
repeatable-read snapshot to proceed without a page-version pin. This slice keeps
the safe page-write skip evidence and fixes the checkpoint-baseline pinning bug.

The performance probe now emits:

- `*_single_owner_skip_blocked_active_pins`;
- `*_single_owner_skip_blocked_baseline`.

These counters separate real peer/snapshot safety fallbacks from the
single-owner skip that should remain hot in ownerless insert loops.

## Test Plan

- Build the production MariaDB embedded archive with
  `tools/mariadb-embedded-build build`.
- Build production embedded MyLite targets with
  `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`.
- Run direct selector
  `mylite_ownerless_cross_process_sql_test single-owner-page-write-refresh-skip`.
- Run CTest
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-page-write-refresh-skip$'
  --output-on-failure`.
- Rerun the active-reader pressure shard:
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-cross-process-sql\.6$' --output-on-failure`.
- Run the stats-enabled reduced production performance probe.
- Run adjacent single-owner selectors and representative ownerless SQL
  coverage.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Acceptance Criteria

- Single-owner insert coverage proves the skip hook is called and allowed, and
  that detailed refresh work remains below the skipped page-write candidates.
- Active-reader pressure coverage proves checkpointed snapshot readers publish
  pins and still block writes at the configured WAL limit.
- Perf counters expose active-pin and missing-baseline skip rejections.
- Production performance counters expose the remaining ownerless autocommit
  cost in page-version reads, page-log append/sync, page-visible publication,
  and MariaDB statement execution.

## Risks And Follow-Up

- The slice does not extend the skip to buffer-pool sweeps or space-header
  refresh entry points; active-reader pressure showed that broader skipping
  needs more targeted proof.
- The slice does not reduce the number of page images published for ownerless
  autocommit writes.
- The slice does not change SQL-level table-lock reachability, active-reader
  page-aware reclaim, or broader DDL/file-lifecycle recovery gaps.
