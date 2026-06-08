# Ownerless Page-Write Refresh Negative Cache

## Problem

The `ownerless-page-write-refresh-profile` slice showed that ownerless
one-row autocommit inserts spend most page-write refresh time proving absence:
for 200 autocommit inserts the probe reported `8176` refresh-function calls,
`7776` page-version misses, `8141` native disk reads, and `0` native disk
overlays. That means MyLite repeatedly performs the same page-version miss and
disk fallback work while the local buffer-pool page is already current for the
active ownerless visibility boundary.

A broad shortcut is unsafe. Earlier page-version miss shortcuts broke CTAS and
DDL coverage because a miss in one lookup context did not prove absence for all
later statement shapes, space/file lifecycle states, or visibility boundaries.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `30e6c181`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` calls
  `mylite_ownerless_innodb_refresh_page_for_write()` before mutating an
  ownerless page unless the current MTR or transaction already owns that page.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  pushes or receives a thread-local `page_visible_lsn` boundary before
  `refresh_page_for_write()` reads page-version WAL records and native disk.
- `refresh_page_for_write()` can prove a negative refresh for a specific page
  only after a page-version lookup reports no usable newer page image and a
  native disk read with matching page identity and valid checksum reports
  `disk_page_lsn <= local_lsn`.
- The hook context is process-local and changes across ownerless runtime opens.
  A negative cache must not survive hook reset/open in a way that can be reused
  for a different database directory even if allocator addresses repeat.

## Design

Add a small 64-entry thread-local direct-mapped negative cache for page-write
refresh.
Each entry stores:

- hook epoch,
- hook callback context pointer,
- tablespace id,
- page number,
- local page LSN observed when the proof was made,
- ownerless visible LSN covered by the proof.

`refresh_page_for_write()` checks the cache only after it knows the current
visible boundary and only when `force_page_version` is false. A hit is accepted
only when all of the following match:

- the current hook epoch,
- the current hook callback context pointer,
- the page identity,
- the local page LSN,
- the cached covered visible LSN is greater than or equal to the current
  `page_visible_lsn`.

On a hit, `refresh_page_for_write()` returns `OK` without allocating the
external page buffer, reading the page-version WAL, locking `fil_system.mutex`,
or falling back to native disk.

The cache stores a proof only when page-version lookup did not produce a newer
overlay candidate and the native disk read validated the same page identity,
passed checksum validation, and had `disk_page_lsn <= local_lsn`. Page-version
errors, identity mismatches, checksum failures, successful overlays, forced
refreshes, missing spaces, missing file nodes, disk read failures, and disk
identity mismatches do not create cache entries.

Hook set/reset increments a process-local cache epoch. This prevents reuse
across ownerless runtime lifetimes even if the callback context address is
reused.

Extend the stats-enabled performance probe with cache hit, miss, and store
counters so CI logs show whether the optimization is active.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, durable metadata, or directory
layout changes. The cache is an internal optimization of the ownerless InnoDB
refresh hook and is disabled for forced page-version refreshes.

The observable behavior must remain the same: peer-visible ownerless writes,
DDL/CTAS refresh, and recovery-sensitive file lifecycle paths must still see
newer page images when they exist.

## Directory And Lifecycle Impact

No new files or durable state. The cache is thread-local and process-local.
Hook epoch binding prevents entries from crossing ownerless runtime lifetimes.

## Native Storage Impact

Native page refresh semantics stay conservative. MyLite only skips a future
refresh attempt after the same thread has already proved that both the
page-version WAL and native disk contain no newer image for the same page,
same local page LSN, and current-or-older visible boundary.

## Build And Performance Impact

Default ownerless refresh gains a cheap direct-mapped cache lookup before the
expensive page-version read and disk fallback. Cache entries are per-thread and
fixed-size; there is no allocation and no new dependency.

The expected performance effect is a reduction in repeated page-version misses
and native disk reads inside the reduced ownerless autocommit insert probe.
The cache will not remove first-time misses, misses after the visible boundary
advances past the cached proof, or forced refreshes.

## Profile Findings

A stats-enabled local probe after implementation reported the following for
the reduced 200-row ownerless autocommit insert phase:

- ownerless autocommit throughput: `103.34 ops/s`,
- MTR page-write refresh bucket: `4588` calls, `145.476ms`,
- refresh-function detail: `8176` calls,
- negative-cache hits: `4310`,
- negative-cache misses: `3866`,
- negative-cache stores: `3613`,
- page-version reads: `3866`, down from the preceding slice's `8176`,
- page-version misses: `3466`, down from `7776`,
- native disk reads: `3815`, down from `8141`,
- native disk overlays: `0`,
- page-publish failures: `0`,
- refresh allocation, page-version checksum, and disk checksum failures: `0`.

The cache removes about half of the repeated refresh miss/fallback work in the
reduced probe while preserving all first-time proofs and forced refreshes. The
remaining misses are still substantial because each autocommit statement can
advance the ownerless visible boundary beyond prior per-page proofs.

## Test Plan

- Build the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_primitives_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run the stats-enabled embedded performance probe and verify cache hits,
  misses, and stores are emitted and page-version/disk read counts do not
  increase.
- Run focused live visibility selectors:
  `prepared-committed-read` and `local-write-first-read`.
- Run DDL/CTAS guard selectors:
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run focused hook negative-proof coverage.
- Run ownerless primitive CTests.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `libmariadbd.a`.
- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe mylite_embedded_open_close_test` passed.
- `build/embedded-dev/packages/libmylite/mylite_embedded_open_close_test`
  exited successfully. It printed the existing MariaDB
  `my_thread_global_end()` thread warning.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and emitted the cache-hit counters recorded above.
- `cmake --build --preset embedded-dev --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test`
  passed.
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

- Stats-enabled probe output reports nonzero negative-cache hits for the
  ownerless autocommit insert phase.
- The probe still reports zero page-publish failures and no refresh checksum or
  allocation failures.
- Focused live-reader and DDL/CTAS selectors pass.
- Hook negative-proof and ownerless primitive CTests pass.
- The optimization remains context/epoch/page/local-LSN/visible-boundary bound
  and does not introduce durable state.

## Risks And Follow-Up

- The cache is intentionally local and conservative. It will not eliminate all
  repeated refresh work while every autocommit statement advances the ownerless
  visible boundary.
- If the measured residual hot path remains dominated by first-time misses,
  the next slice should investigate statement-scoped page write batching or a
  stronger page-version index proof, not broaden this cache beyond its proof
  boundary.
