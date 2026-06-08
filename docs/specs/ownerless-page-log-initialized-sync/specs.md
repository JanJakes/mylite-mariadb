# Ownerless Page Log Initialized Sync

## Problem

Ownerless autocommit writes still spend substantial time in MariaDB/InnoDB
commit execution. The previous page-publish type probe showed the largest
remaining write-volume problem is native-support page publication, not header
validation. However, the hot page-visible commit path still validates and reads
the page-log header before every ownerless page-log data sync:

- the ownerless runtime initializes `mylite-concurrency.wal` before installing
  InnoDB hooks;
- `ownerless_innodb_pages_visible_hook()` syncs that already-initialized log
  for every page-visible commit;
- `mylite_ownerless_page_log_sync_at()` remains the conservative public helper
  and revalidates the header on every call.

This slice removes the repeated header read from the initialized ownerless
runtime sync path without changing the conservative public API.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc`
  `mylite_ownerless_page_log_sync_at()` takes the page-log read lock, validates
  the existing header, then calls the existing data-sync helper.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_pages_visible_hook()` calls page-log sync after dirty page
  images have been published and before it publishes the visible LSN in shared
  redo state and persists the checkpoint.
- `mylite_ownerless_page_log_append_initialized_at()` already provides the
  same initialized-runtime pattern for page-version appends.

## Design

Add `mylite_ownerless_page_log_sync_initialized_at()`:

- keep the same read-lock acquisition and data-sync helper as
  `mylite_ownerless_page_log_sync_at()`;
- check that the initialized page-log header range exists at the requested
  offset;
- skip the repeated header read and magic/version validation;
- keep `mylite_ownerless_page_log_sync_at()` conservative for callers that need
  validation.

The ownerless InnoDB page-visible hook uses the initialized helper because the
runtime opened and initialized the page log before hooks were installed.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, storage-format, or page-log format
changes. The new helper is internal first-party API used by the ownerless
runtime path.

## Directory And Lifecycle Impact

No directory-layout change. The sync target remains
`concurrency/mylite-concurrency.wal` inside the MyLite database directory.

## Native Storage Impact

No native InnoDB format or commit-ordering change. Page-visible publication
still syncs the page log before publishing the visible LSN and checkpoint.

## Build And Performance Impact

The expected impact is small but safe: the hot page-visible path avoids one
header read/validation per page-visible commit. The larger ownerless
autocommit gap remains the native-support page publish volume and commit-path
work recorded by the page-publish type probe.

## Test Plan

- Add primitive page-log coverage proving initialized sync fails before the log
  header exists, succeeds after `initialize_at()`, succeeds after initialized
  append, and leaves the conservative validating sync API working.
- Rebuild production embedded targets.
- Run ownerless primitives, focused ownerless SQL selectors, production
  ownerless stress, production negative-proof hooks, a reduced production
  performance probe, `format-check-prod`, and `git diff --check`.

## Verification Results

Local production verification on 2026-06-08:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test
  mylite_embedded_performance_probe` passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-primitives$' --output-on-failure` passed in `2.93s`.
- Direct focused selectors passed:
  `prepared-committed-read`, `local-write-first-read`, `native-reclaim`,
  `live-reclaim`, `commit-race`, and `active-reader-pressure`.
- A reduced production performance probe with page-publish stats enabled
  passed; the autocommit sample reported ordinary `1788.19 ops/s`, ownerless
  `206.17 ops/s`, `page_publish_published=3203`,
  `page_publish_native_support=2803`, `page_publish_snapshot_boundary=400`,
  and `pages_visible_hook_sync_ms=5.853` for 400 ownerless autocommit inserts.
- `ctest --preset ownerless-stress --output-on-failure` passed all 12
  production stress cases in `173.76s`.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed all 3 production unsafe-hook cases in `22.49s`.
- `cmake --build --preset format-check-prod && git diff --check` passed.

## Acceptance Criteria

- The ownerless page-visible hook uses the initialized sync helper.
- Existing page-log primitive and ownerless SQL coverage keeps passing.
- Production performance probes still emit ownerless autocommit timings and
  page-visible sync counters.

## Risks And Follow-Up

- This does not reduce page-version append count.
- The next larger optimization should target native-support page publication
  or commit-path batching, with crash and peer-visibility tests before any
  appends or syncs are skipped.
