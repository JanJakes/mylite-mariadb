# Ownerless Transaction Image In-Place Prepare

## Problem Statement

Current ownerless autocommit attribution shows the remaining write slowdown in
the native insert step, with visible cost in row-level mini-transaction commit
and ownerless commit visibility. Transaction-deferred page publication already
captures a private page image while the page is latched, but the transaction
commit publisher allocates a second page-sized `std::vector`, copies the
captured image into it, prepares InnoDB page checksums there, and immediately
passes that throwaway copy to the page-version hook.

The captured image belongs to the committing transaction and is cleared during
transaction cleanup. Preparing that private buffer in place removes one
page-sized allocation and copy from every captured transaction-image publish
without changing which page images are published.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:3151` records transaction-deferred
  page images in `trx->mylite_ownerless_page_images` while the source page is
  still latched. A later capture for the same packed page id replaces older
  image contents when its page LSN is not older.
- `mariadb/storage/innobase/trx/trx0trx.cc:2098` publishes deferred
  transaction page images during `trx_t::commit_in_memory()` before the
  visible-fast commit decision can skip the conservative dirty-page flush.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:1679`
  implements `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()`.
  Before this slice it copied each captured image into a temporary vector,
  prepared the temporary page for writing with `buf_flush_init_for_writing()`
  or `buf_flush_update_zip_checksum()`, and passed the temporary buffer to
  `mylite_ownerless_innodb_publish_page_version()`.
- `mariadb/storage/innobase/include/trx0trx.h:722` clears
  `mylite_ownerless_page_images` during transaction cleanup, and
  `mariadb/storage/innobase/trx/trx0trx.cc:553` deletes the vector when the
  transaction object is destroyed.

## Design

Publish captured transaction images from their existing transaction-owned page
buffer:

- iterate mutable captured image entries during
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()`;
- keep the existing validation that skips zero-LSN, zero-size, or malformed
  captured images;
- prepare the captured buffer in place with the same InnoDB checksum helper
  that previously prepared the temporary copy;
- pass the same prepared bytes to `mylite_ownerless_innodb_publish_page_version()`;
- keep the successful-image page-id set and existing page-id fallback loop
  unchanged.

The in-place mutation is safe because the captured image is private to the
transaction commit path, the publisher is the only consumer after capture, and
transaction cleanup clears the image vector after commit. If image publication
fails, the existing page-id fallback still tries to publish from the buffer
pool path.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli, wire-protocol, metadata, page-log record
format, checkpoint format, directory layout, or storage-engine behavior
changes. The same page image is still prepared with InnoDB's existing checksum
helpers before page-version publication.

## Native Storage Impact

Native InnoDB pages, redo, undo, rollback-segment history, page-version WAL,
checkpoint ordering, and recovery behavior are unchanged. The slice removes a
temporary copy inside the ownerless transaction-image publisher only.

## Build And Performance Impact

The MariaDB embedded archive must be rebuilt because the change touches
`mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`.

Expected impact is a small reduction in ownerless commit visibility and
row-level MTR commit overhead for workloads that publish transaction-deferred
page images. It does not reduce page-version count, history-proof native-support
page publication, page-log payload size, or broader native redo/checkpoint
work.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive with the production `MinSizeRel`
  baseline.
- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_cross_process_sql_test`, and focused ownerless primitive
  coverage under `php-embedded-prod`.
- Run focused ownerless SQL selectors covering visible-fast commit,
  native-support WAL proof, uncommitted peer hiding, live reclaim, and
  commit-race.
- Run a reduced stats-enabled production attribution probe and compare
  transaction-image counts, page-log append counts, ownerless commit visibility,
  and client step timing.
- Run a reduced stats-off production throughput probe.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Captured transaction image publication no longer allocates a second
  page-sized vector per image.
- Transaction-image publish attempts and successful-image page-id fallback
  behavior remain unchanged for the focused probe shape.
- Failed or malformed captured images still use the existing page-id fallback.
- Focused ownerless correctness coverage passes.
- Docs record this as a bounded allocation/copy cleanup, not a completion of
  ownerless write-throughput or native redo/checkpoint work.

## Verification Results

Local production verification rebuilt the MariaDB embedded archive and focused
MyLite targets:

```sh
tools/mariadb-embedded-build build
cmake --build --preset php-embedded-prod --target \
  mylite_embedded_performance_probe \
  mylite_ownerless_cross_process_sql_test \
  mylite_ownerless_primitives_test
```

Focused ownerless primitive and SQL selectors passed:

```sh
ctest --preset php-embedded-prod -R \
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$' \
  --output-on-failure

ctest --preset php-embedded-prod -R \
  '^libmylite\.ownerless-single-owner-(foreground-reclaim-budget|foreground-reclaim-peer-history|history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$' \
  --output-on-failure

build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test \
  commit-race
build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test \
  live-reclaim
build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test \
  explicit-transaction-visible-fast-commit
```

A reduced stats-enabled production probe used:

```sh
env MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
  MYLITE_PERF_SELECT_ITERATIONS=100 \
  MYLITE_PERF_INSERT_ITERATIONS=100 \
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The sample preserved the expected publication shape while reducing allocation
and copy work inside transaction-image publication:

- transaction-image publishes: `1.020` per insert;
- transaction-buffer fallback publishes: `0.020` per insert;
- page-log append calls: `3.040` per insert;
- page-write publish calls: `3.000` per insert;
- ownerless page-log append: `0.064 ms/insert`;
- ownerless page-publish hook: `0.082 ms/insert`;
- ownerless page-write publish copy: `0.003 ms/insert`;
- ownerless client step: `0.817 ms/insert`;
- ownerless autocommit throughput: `1222.17 ops/s` versus ordinary
  `3483.44 ops/s`, ratio `0.3509`.

A reduced stats-off production probe used:

```sh
env MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
  MYLITE_PERF_SELECT_ITERATIONS=100 \
  MYLITE_PERF_INSERT_ITERATIONS=500 \
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported ownerless explicit transactions at `2942.32 ops/s` versus ordinary
`4510.83 ops/s`, ratio `0.6523`; ownerless autocommit at `1507.56 ops/s`
versus ordinary `3877.29 ops/s`, ratio `0.3888`; and ownerless four-row bulk
rows at `5158.73 ops/s` versus ordinary `12776.96 ops/s`, ratio `0.4038`.
These short reduced samples are directional performance evidence only; the
contractual proof is unchanged publication counts and passing correctness
coverage.

## Risks And Follow-Up

The captured image buffer is no longer a pristine latch-time copy after the
transaction publisher prepares it for WAL publication. That is acceptable for
the current lifetime because the image is private to the transaction publisher
and is cleared during transaction cleanup. If a future rollback, retry, or
external diagnostic path needs the unprepared latch-time bytes after publish,
it must either read before this preparation point or make its own copy.

Larger remaining work stays in native history-proof publication volume,
transaction-deferred page publication proof, row-level MTR commit cost, and
broader redo/checkpoint reconciliation.
