# Ownerless Page-Write Refresh Profile

## Problem

The WordPress PHPUnit timing split proved that ordinary PHP/mysqli work is not
the current branch-specific regression. The remaining measured gap is
ownerless one-row autocommit `INSERT`: the reduced embedded performance probe
still reports ownerless autocommit throughput around tens of operations per
second while ordinary autocommit remains in the low thousands.

The existing ownerless diagnostics identify page-write refresh as one large
bucket, but they do not explain what the refresh path did. That makes the next
optimization unsafe to choose: a refresh call may have copied a newer
page-version record, missed the page-version WAL and fallen back to native
disk, skipped an unsupported page class, or done no overlay after proving the
local page was already current.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `a27aad22`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` calls
  `mtr_t::ownerless_page_write_refresh()` from the page-write enter path.
  The current opt-in `ownerless_page_write_perf_stats` only counts refresh
  calls and total refresh time.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements `mylite_ownerless_innodb_refresh_page_for_write()` and
  `mylite_ownerless_innodb_refresh_page_for_write_force()`. Both observe the
  ownerless redo state when possible, then call `refresh_page_for_write()`.
- `refresh_page_for_write()` always allocates an aligned external page buffer
  for publishable uncompressed file pages. It can push page visibility, lock
  `fil_system.mutex`, look up the tablespace and file node, read the
  ownerless page-version WAL through `mylite_ownerless_innodb_read_page_version()`,
  overlay a verified page-version image, or fall back to reading the native
  tablespace file and overlay only if the disk page LSN is newer.
- Prior unsafe broad miss shortcuts in the page-version index path broke CTAS
  and DDL coverage. The page-write refresh path needs outcome-specific evidence
  before MyLite can skip or cache any part of it.

## Design

Add opt-in internal counters for `refresh_page_for_write()` and surface them
through the existing stats-enabled embedded performance probe. The counters
are disabled by default and are only enabled when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, alongside the existing ownerless
page-publish, commit-visibility, database, and MTR page-write timing buckets.

The new counters record:

- total refresh calls,
- forced and current-visibility calls,
- unsupported page skips,
- allocation failures,
- visibility-push calls and errors,
- missing tablespace or file-node skips,
- page-version read calls, hits, misses, errors, identity mismatches,
  not-newer outcomes, checksum failures, overlays, and read time,
- native disk read calls, failures, identity mismatches, not-newer outcomes,
  checksum failures, overlays, and read time,
- page-0 space-header refreshes.

The slice intentionally does not alter page-refresh behavior. It only adds
diagnostics needed to choose a later optimization around redundant refresh,
page-version miss handling, disk fallback, or page-image publication.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, durable file, locking, or
recovery behavior changes. The new symbols are internal test/probe hooks and
are not part of the documented `libmylite` API.

## Directory And Lifecycle Impact

No new durable files or directory-layout changes. The counters are process-local
atomics and reset by the performance probe before each measured ownerless insert
phase.

## Native Storage Impact

The refresh path still preserves the existing conservative overlay rules:
unsupported page classes are skipped, page-version images must match identity
and checksum before overlay, and native disk images overlay only when their
page LSN is newer than the local buffer-pool page.

## Build And Performance Impact

Default runtime cost is one relaxed boolean check per counted branch only when
the function reaches the new helper calls. The atomics and timing reads are
active only for the opt-in probe mode.

The expected immediate value is more precise CI and local profiling evidence:
the probe should show whether ownerless autocommit refresh time is dominated by
page-version misses, successful page-version overlays, native disk fallbacks,
or no-op freshness checks.

## Profile Findings

A stats-enabled local probe after implementation reported the following for
the reduced 200-row ownerless autocommit insert phase:

- ownerless autocommit throughput: `66.87 ops/s`,
- MTR page-write refresh bucket: `4588` calls, `216.673ms`,
- refresh-function detail: `8176` calls,
- current-visibility refreshes: `4988`,
- visibility-push refreshes: `3188`,
- page-version reads: `8176`, `174.945ms`,
- page-version hits: `400`,
- page-version misses: `7776`,
- page-version overlays: `35`,
- page-version not-newer outcomes: `365`,
- native disk reads: `8141`, `32.301ms`,
- native disk identity mismatches: `202`,
- native disk not-newer outcomes: `7939`,
- native disk overlays: `0`.

The detail counter is intentionally broader than the existing MTR bucket:
`refresh_page_for_write()` is also reached from non-MTR ownerless refresh
helpers during the measured phase. The result still isolates the dominant
shape: almost every autocommit refresh misses the page-version WAL, falls back
to a native disk read, and then finds no newer disk image to overlay.

The next optimization should target redundant page-version miss plus native
disk fallback work, but only with a scope gate that preserves DDL/CTAS,
file-lifecycle recovery, and active-reader visibility. The prior
page-version-index miss shortcut failure is direct evidence that a broad
"miss means absent forever" shortcut is not safe.

## Test Plan

- Build the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_primitives_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run the stats-enabled embedded performance probe and verify it emits the new
  refresh counters for transactional and autocommit ownerless inserts.
- Run focused ownerless live visibility selectors:
  `prepared-committed-read` and `local-write-first-read`.
- Run DDL/CTAS guard selectors that previously caught unsafe page-version miss
  shortcuts: `ctas-post-create-dml`, `ddl-broader`, and
  `online-ddl-options`.
- Run focused ownerless primitive CTests.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `libmariadbd.a`.
- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe mylite_ownerless_primitives_test
  mylite_ownerless_cross_process_sql_test` passed.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and emitted the refresh detail counters recorded above.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  local-write-first-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  ctas-post-create-dml` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  ddl-broader` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  online-ddl-options` passed.
- `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- The performance probe emits page-write refresh outcome counters whenever
  ownerless stats are enabled.
- Counters reset between the transactional and autocommit insert phases.
- Default non-stats behavior is unchanged.
- Focused ownerless visibility, DDL guard, and primitive tests pass.
- The spec records the measured refresh mix so the next optimization can target
  a proven dominant path.

## Risks And Follow-Up

- Instrumentation can identify a dominant path but does not by itself close
  the ownerless autocommit gap.
- A later optimization still needs separate correctness evidence for active
  readers, native DDL/file lifecycle, and crash recovery before skipping
  refresh, page-version reads, disk fallbacks, or page-version publication.
