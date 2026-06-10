# Ownerless Transaction Image Publish Dedup

## Problem

Production autocommit attribution shows more page-version WAL appends than the
MTR page-publish counters report. The latest stats-enabled sample for 100
ownerless autocommit inserts reported `300` MTR-published page versions, but
`738` page-log append calls and `739` first-party page-publish hook calls. The
extra volume comes from transaction-page publication and native dirty-page
publish paths that also call `mylite_ownerless_innodb_publish_page_version()`.

Captured transaction page images are authoritative page snapshots taken while
the page is still latched. Once such an image is successfully published for a
page at transaction commit, the later page-id fallback publish for the same
page is redundant work.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_capture_dirty_transaction_page()` records the
  latest captured image for each transaction-owned page. Existing code replaces
  an older captured image when a later page LSN is seen for the same packed
  `space_id:page_no`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` first publishes
  every captured image in `trx->mylite_ownerless_page_images`, then loops over
  the transaction page-id set from `collect_transaction_page_write_pages()` and
  calls `buf_flush_publish_ownerless_page_to_lsn()` for each page id.
- `collect_transaction_page_write_pages(trx, pages, false)` collects
  transaction-modified page ids and deduplicates the page-id vector, but it
  does not know which of those page ids already had a captured page image
  published successfully.
- `trx_t::commit_in_memory()` calls
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` before deciding
  whether visible-only publication can skip the dirty-page flush bridge.

## Design

Keep the captured-image publish path first. Track page ids whose captured image
publish returns `MYLITE_OWNERLESS_INNODB_LOCK_OK`. Before running the page-id
fallback loop, sort and deduplicate that successful-image page-id list. The
page-id fallback loop skips only pages found in that successful-image list.

The existing fallback remains active when:

- a page has no captured image;
- a captured image is malformed and skipped;
- publishing the captured image returns anything other than
  `MYLITE_OWNERLESS_INNODB_LOCK_OK`;
- a page appears only in the page-id vector.

The return value still advances to the maximum publish LSN observed from
captured images. Skipping the page-id fallback for a successfully published
image therefore preserves the visible LSN handoff that already happens after
image publication.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, wire-protocol, metadata, directory layout, or
storage-format behavior changes. The change removes redundant internal
page-version publication work after an equivalent captured image has already
been accepted by the page-version hook.

## Directory And Lifecycle Impact

No new files are created. Page-version WAL record format, checkpointing,
replay, page-index publication, and `.shm` rebuild behavior are unchanged.

## Native Storage Impact

No native InnoDB page, redo, undo, or rollback-segment format changes. Native
fallback publication remains the authority for pages without a successful
captured image.

## Build And Performance Impact

The MariaDB embedded archive must be rebuilt after changing the ownerless
InnoDB lock hook file. The intended performance effect is fewer duplicate
page-version hook calls and page-log appends during transaction commit. The
slice does not reduce MTR-published history pages, row-insert work, or page-log
payload size for genuinely distinct page versions.

## Test And Verification Plan

- Verify `build/mariadb-embedded` is `MinSizeRel` and
  `build/php-embedded-prod` is `Release`.
- Rebuild the MariaDB embedded archive.
- Build production `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test`.
- Run focused ownerless visibility/history selectors and direct `commit-race`.
- Run reduced stats-enabled production attribution and compare page-log append
  calls, page-publish hook calls, and ownerless autocommit throughput against
  the pre-slice evidence.
- Run reduced stats-off production throughput.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Acceptance Criteria

- A successful captured image publish suppresses only the later page-id
  fallback for the same packed page id.
- Failed or unavailable image publishes still fall back through the existing
  page-id publish path.
- Focused production ownerless correctness coverage passes.
- Production attribution shows lower page-version hook/append volume or
  documents why the candidate did not reduce the measured path.

## Verification Results

Local verification on 2026-06-10 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/mariadb-embedded-build build` passed after the InnoDB source change
  and rebuilt `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-(primitives|single-owner-(history-wal-proof|native-support-page-wal-elision)|uncommitted-peer-hidden)$'
  --output-on-failure` passed.
- Direct ownerless SQL selectors passed for `prepared-committed-read`,
  `local-write-first-read`, `isolation`, `live-reclaim`, and `commit-race`.
- A reduced stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported ownerless autocommit
  at `1051.34 ops/s`. Page-publish hook calls fell from the previous
  `739` per 100 inserts to `303`, and page-log append calls fell from `738`
  to `302`, while MTR page-publish evidence stayed at `457` candidates,
  `300` published records, and `157` native-support elisions. Page-log append
  time fell to `0.074 ms/insert`, and the page-publish hook summary fell to
  `0.094 ms/insert`.
- A stats-off production throughput sample with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ordinary autocommit at
  `2051.69 ops/s`, ownerless autocommit at `652.98 ops/s`, ownerless
  autocommit ratio `0.3183`, ordinary transaction inserts at
  `2064.80 ops/s`, ownerless transaction inserts at `1589.52 ops/s`, and
  ownerless transaction ratio `0.7698`.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Unresolved Questions

- The optimization assumes the captured image vector keeps the latest page
  image for each transaction-owned page. The current code explicitly replaces
  an older image when a later page LSN is captured.
- This does not address page-version volume from native dirty-page bridge,
  DDL/dictionary paths, rollback, or active-reader boundary synthesis.
