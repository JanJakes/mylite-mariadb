# Ownerless Native-Support Live Index Skip

## Problem

Ownerless autocommit inserts still publish about three page-version records per
single-row insert. The remaining records are useful durability and recovery
evidence, but proof-only native-support records do not contain materializable
page payloads and must not be treated as live page-index images. Full
native-support page images can be indexed like other materializable page
versions; proof-only records stay in the WAL for durability and recovery proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` can use ownerless WAL proof records
  for the rollback-segment and undo pages before skipping the exact native
  history flush.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_publish_type_has_native_support()` identifies InnoDB
  native-support page classes, and
  `ownerless_page_write_can_elide_native_support_page()` already elides support
  pages only when they are not required as active history proof.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_page_publish_hook()` appends materializable page images to
  `mylite-concurrency.wal` and publishes their record offsets into the live
  shared page index. Proof-only native-support records are appended without a
  live page-index image entry.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_page_read_locked()` treats the page index as a cache. Index
  miss, stale, or scan-required states fall back to scanning the WAL under the
  page-log read lock. Its negative cache is bounded by the page-log snapshot end
  offset, so later appended records extend the scan range.
- `packages/libmylite/src/database.cc` `replay_concurrency_page_index()` rebuilds
  the shared page index by replaying materializable page-version WAL records,
  including full native-support page images; proof-only records are skipped by
  page-log replay.

## Design

Keep native-support proof records in the ownerless page-version WAL. After a
successful proof-only append, skip the live
`mylite_ownerless_page_index_publish()` call because the record has no page
payload. Full native-support page images still publish their live page-index
entry so single-owner readers can treat index absence as an absence proof for
materializable page versions.

This preserves:

- history-proof durability, because the WAL record is still appended before the
  hook returns success;
- crash recovery and `.shm` rebuild, because replay still indexes every
  materializable WAL record and still skips proof-only metadata;
- live reads, because the page index is a cache and a miss falls back to WAL
  scan;
- native snapshot-boundary behavior, because native-support pages already skip
  synthesized active-reader boundary probes.

Add a private database performance counter for skipped proof-only
native-support live-index publishes and expose it in the production embedded
performance probe. The counter is intentionally process-local diagnostic state,
not a public C API.

## Scope And Non-Goals

In scope:

- first-party `libmylite` ownerless publish hook behavior;
- production embedded performance probe attribution;
- focused ownerless SQL regression coverage for native-support proof pages and
  forced shared-memory rebuild.

Out of scope:

- reducing the number or payload size of native-support WAL records;
- changing the page-version WAL format;
- changing checkpoint, redo, undo, or InnoDB native file formats;
- claiming ownerless write performance is now close to trunk.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, directory-layout, or storage-engine file
format behavior changes. Page-version reads skip proof-only native-support
records as non-images, while full native-support page images remain readable by
index or WAL scan.

## Directory And Lifecycle Impact

No new files are introduced. Existing `mylite-concurrency.wal` durability and
`mylite-concurrency.shm` rebuild rules remain unchanged.

## Native Storage Impact

Native InnoDB pages and redo/checkpoint state are unchanged. The slice removes
only a live shared-memory cache update for proof-only native-support records
after their WAL append succeeds.

## Build And Performance Impact

The expected performance effect is bounded: it removes live page-index
publication for proof-only native-support records, not the WAL append itself. The
production probe reports
`mylite_perf_summary_ownerless_autocommit_page_publish_index_skipped_native_support_per_insert`
so future CI timing can distinguish skipped index work from remaining WAL and
commit-path cost.

## Test Plan

- Build production embedded `libmylite`, the performance probe, and ownerless
  cross-process SQL test.
- Run the focused native-support page WAL elision SQL selector. It verifies
  native-support records are still published, the live-index skip counter covers
  proof-only records, normal reopen works, and forced `.shm` rebuild preserves
  data.
- Run focused history-proof and ownerless visibility selectors.
- Run a reduced stats-enabled production performance probe and verify the new
  raw and summary keys appear with native-support skips.
- Run ownerless hook crash selectors around page publication and production
  build/static checks.

## Acceptance Criteria

- Native-support proof-only records are still appended to the WAL.
- Native-support proof-only records no longer update the live page index on the
  commit hot path.
- Page-version WAL scan and replay continue to recover committed data after
  forced shared-memory rebuild.
- Production probe output exposes the skipped live-index count.

## Verification Results

Local verification on 2026-06-17 used production artifacts from
`build/php-embedded-prod` and hook artifacts from `build/ownerless-test-hooks`.

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- `mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision` passed and asserted the skipped
  live-index counter covers the published proof-only native-support records.
- Focused adjacent selectors passed:
  `single-owner-history-wal-proof`, `prepared-committed-read`, and
  `active-pin-reclaim-boundary`.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=300`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported `3.000` page-version
  records per insert, `2.000` published native-support pages per insert,
  `2.010` skipped native-support live-index publishes per insert,
  `0.002 ms/insert` in page-publish index time, `0.054 ms/insert` in page-log
  append time, and ownerless autocommit ratio `0.4486`.
- A stats-off production throughput probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=2000` reported ownerless/ordinary ratios:
  direct `SELECT 1` `0.6966`, prepared `SELECT 1` `0.6564`, transaction inserts
  `0.5847`, single-row autocommit inserts `0.4857`, and bulk insert rows
  `0.2936`. The single-row autocommit gap therefore remains substantial.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- Hook selectors `page-publish-before-append-crash`, `visible-publish-crash`,
  and `visible-checkpoint-crash` passed. The attempted
  `page-publish-after-append-crash` selector is not registered in this binary.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure` passed in
  `233.26 sec`.
- `tools/check-ci-production-builds`, `cmake --build --preset
  format-check-prod`, and `git diff --check` passed.

## Risks

- This is not the main remaining write-path fix. Single-row autocommit will
  still pay for native-support WAL append and InnoDB commit work until broader
  redo/checkpoint/native-history proof can safely reduce the remaining page
  publication volume.
- Live readers that need a skipped proof-only native-support record do not get a
  page image from that record; full native-support page images are indexed and
  remain readable by index or WAL scan.
