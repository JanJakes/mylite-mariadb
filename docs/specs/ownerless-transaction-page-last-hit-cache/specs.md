# Ownerless Transaction Page Last-Hit Cache

## Problem

The remaining ownerless bulk-insert performance gap is concentrated in the
non-empty-table row insert and undo-report path. The existing transaction page
membership cache removes linear scans once the ownerless modified, dirty, or
native-support page vectors reach the set threshold, but the hot 100-row bulk
shape repeatedly asks about the same small set of pages before those vectors
need a hash set.

This slice reduces that exact membership overhead without changing native
InnoDB undo, redo, rollback, page-version publication, or ownerless lock
ownership.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0rec.cc::trx_undo_report_row_operation()`
  still creates one undo report mini-transaction per later non-empty-table
  inserted row. MariaDB's `TRX_UNDO_EMPTY` path remains limited to the first
  statement inserting into an empty table.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::ownerless_page_write_enter()`,
  `ownerless_page_write_note_dirty_transaction_page()`, and the native-support
  lock-hold path repeatedly call exact transaction page membership helpers.
- `mariadb/storage/innobase/trx/trx0trx.cc` stores authoritative ownerless
  modified, dirty, and native-support page-write vectors, with lazy exact
  membership sets once a vector reaches `16` entries.
- The prior `ownerless-transaction-page-membership-cache` slice kept those
  vectors authoritative and explicitly preserved page-version WAL, checkpoint,
  recovery, and native InnoDB page format.

## Design

Add one transaction-local positive last-hit cache for each ownerless exact
membership class:

- modified pages,
- dirty pages, and
- native-support page-write locks held to transaction cleanup.

The cache only returns `true` after a previous exact vector/set lookup or a
successful append to the authoritative vector established the same packed page.
Misses still fall through to the existing vector/set helpers. The cache is
cleared whenever the matching authoritative vectors are cleared and the
modified-page cache is also cleared before rebuilding the modified-page set
after vector erasure.

No negative results are cached. No durable state, shared-memory field, lock
record, page-version record, redo record, or undo record changes.

## Compatibility Impact

No SQL, C API, PHP/mysqli, storage-format, directory-layout, or recovery
behavior changes. This is a process-local exact membership cache used only by
ownerless transaction bookkeeping.

The slice deliberately does not broaden non-empty-table bulk insert undo
elision. Later statements still require MariaDB row-level undo for statement
rollback, savepoint, consistent-read, and purge semantics.

## Native Storage And Lifecycle Impact

Native InnoDB storage remains unchanged. Ownerless page-write locks are still
acquired, held, released, and published by the existing paths. The new fields
live only inside `trx_t` and are reset with the existing ownerless transaction
page-vector lifecycle.

## Test And Verification Plan

- Rebuild the production MariaDB embedded archive after editing
  `mariadb/storage/innobase`.
- Build ownerless SQL and performance probe targets with `php-embedded-prod`.
- Run focused ownerless selectors for visible-fast multi-row inserts,
  native-support WAL elision, history proof, and uncommitted peer visibility.
- Run a reduced production ownerless performance probe and compare bulk
  throughput plus ownerless row/undo attribution.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Last-hit caches are positive-only exact caches.
- Clearing ownerless modified/dirty/native-support page vectors invalidates the
  matching last-hit cache.
- Rebuilding the modified-page membership set after vector erasure cannot leave
  a stale positive last hit.
- Existing focused ownerless correctness selectors pass.
- The change is documented as a bounded hot-path cleanup, not ownerless
  concurrency completion.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- focused direct SQL selectors: `commit-race`, `active-reader-pressure`, and
  `prepared-committed-read`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-ownerless-innodb-lock-hooks|embedded-ownerless-product-hooks)$'
  --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-single-owner-native-support-page-wal-elision|ownerless-single-owner-multi-row-insert-visible-fast-path|ownerless-single-owner-history-wal-proof|ownerless-negative-proof|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test && ctest --preset ownerless-stress
  --output-on-failure`

The reduced stats-off production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=5000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported ownerless 100-row bulk rows at `36020.90 rows/s` and an
ownerless/ordinary rows ratio of `0.3538`, compared with the previous local
stats-off sample ratio of `0.3260`.

The reduced stats-enabled production attribution probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported ownerless bulk `mysql_query()` at `3.071 ms/statement`, remaining
non-empty-table row insert at `2.049 ms/statement`, remaining
undo-report MTR commit at `0.543 ms/statement`, and an ownerless/ordinary bulk
rows ratio of `0.4364`. The previous local stats-enabled sample reported
`3.229 ms/statement`, `2.099 ms/statement`, `0.583 ms/statement`, and
`0.3685`, respectively.

## Risks And Follow-Up

- This only removes small-vector repeat lookup overhead. It does not remove the
  deeper MariaDB row-level undo work that remains necessary for non-empty-table
  statements.
- Broader native redo/checkpoint reconciliation, DDL/file-lifecycle recovery,
  active-reader pressure policy, and external MariaDB/RQG stress remain planned
  ownerless completion work.
