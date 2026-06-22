# Ownerless Dirty Publish Append Batch

## Problem

Production-mode ownerless bulk insert performance still trails ordinary InnoDB
after the CI timing split and production-build guard work. A local optimized
100-row bulk attribution run on `7472a6f5f` showed the ownerless remaining-row
bulk ratio near `0.20x` with append stats enabled. The same run attributed about
`1.737 ms/statement` to page-log append work, with roughly `58.700` page-log
append records per 100-row statement and `54.400` direct append calls per
statement.

Direct appends repeat append-lock, file-stat, and header setup work that the
existing page-log append session can amortize. Transaction-deferred page
publication already uses the page-publish batch scope, but dirty-scan and
buffer-pool publication paths did not.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` calls
  `mylite_ownerless_innodb_publish_dirty_pages_to_lsn()` during ownerless commit
  visibility when dirty native-support pages must be published before visible
  LSN publication.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` already wraps
  transaction-deferred page publication in `ownerless_page_publish_batch_scope`.
- The same hook file routed `mylite_ownerless_innodb_publish_dirty_pages_to_lsn()`
  directly to `buf_flush_publish_ownerless_pages_to_lsn()`, and
  `mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn()` looped over pages
  without a page-publish batch scope.
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_publish_ownerless_pages_to_lsn()` collects publishable dirty pages
  under buffer-pool locks, then publishes copied page images after releasing
  those locks. Adding a hook-level append batch does not extend buffer-pool latch
  lifetimes.
- `packages/libmylite/src/database.cc`
  `append_ownerless_page_version()` uses the thread-local page-log append session
  when `ownerless_innodb_page_publish_batch_begin_hook()` has set the matching
  hook context. Batch end releases the session unless the statement explicitly
  defers release to page-visible publication.

## Design

Wrap dirty-scan and full buffer-pool ownerless page publication in the existing
page-publish batch scope:

- `mylite_ownerless_innodb_publish_dirty_pages_to_lsn()` starts a batch before
  calling `buf_flush_publish_ownerless_pages_to_lsn()`.
- `mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn()` starts a batch
  around the collected page loop when at least one page is present.

The slice does not change page eligibility, page contents, record ordering,
page-visible publication, redo completion, checkpoint persistence, SQL behavior,
public C API behavior, or the page-log record format. It only lets the existing
append session carry a group of page-version records that were already published
back-to-back by the same hook context.

## Compatibility Impact

MySQL/MariaDB SQL semantics are unchanged. The affected behavior is internal
ownerless page-version WAL publication for native InnoDB dirty pages inside the
single MyLite database directory.

## Database Directory And Native Storage Impact

No new files or durable paths are introduced. Page-version records remain in the
existing MyLite-owned ownerless page log, and native InnoDB pages remain in their
MariaDB file formats.

## Build, Size, And Dependency Impact

The change reuses existing hook and page-log batching code. It adds no
dependency and has negligible binary-size impact.

## Verification Plan

- Rebuild the optimized embedded target after the MariaDB-derived hook change.
- Run the focused embedded ownerless hook test.
- Run focused ownerless SQL selectors that exercise visible-fast insert,
  native-support WAL elision, and history WAL proof behavior.
- Run append attribution before and after the change and compare direct append
  calls, session append calls, page-log append time, and headline bulk ratio.
- Run production-build guards and whitespace checks before commit.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

Append-attribution command:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_APPEND_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

Before this slice, the same local optimized run reported:

- direct page-log append calls: `54.400` per statement;
- session append calls: `4.300` per statement;
- page-log append calls: `58.700` per statement;
- page-log append time: `1.737 ms/statement`;
- page-log encode time: `1.013 ms/statement`;
- ownerless/ordinary remaining bulk-row ratio: `0.1982`.

After this slice, append attribution reported:

- direct page-log append calls: `0.000` per statement;
- session append calls: `31.500` per statement;
- page-log append calls: `31.500` per statement;
- page-log append time: `0.720 ms/statement`;
- page-log encode time: `0.497 ms/statement`;
- ownerless/ordinary remaining bulk-row ratio: `0.1907`.

Two stats-off production samples after the change remained noisy, with remaining
bulk-row ratios of `0.1535` and `0.2693`. This slice therefore records a clear
append-path reduction, not a broad throughput claim.

## Acceptance Criteria

- Dirty-scan and full buffer-pool page publication use the existing page-log
  append session when records are published by the same ownerless hook context.
- Focused ownerless correctness coverage still passes.
- Production append attribution shows direct append calls decrease for the
  100-row bulk insert shape without reducing page-version record coverage.

## Risks

Batching keeps the append session open across more page publications in the same
hook call. The page images are copied before publication and the batch scope is
outside buffer-pool critical sections, so this should not lengthen native page
latch ownership. The scope also relies on existing batch-end behavior to release
the session before page-visible publication when statement-level deferral is not
active.
