# Ownerless Native-Support Lock Hold

## Problem

Stats-enabled ownerless bulk attribution shows later non-empty-table row-list
statements spending most ownerless overhead in native row insert and undo
reporting. The page-write subcounters also show repeated ownerless page-write
enter, acquire, leave, and release work around native-support pages whose page
images are already eligible for native-support WAL elision.

A tempting optimization is to skip native-support page-write locks when a
single-owner epoch is observed. That is unsafe: the single-owner observation is
not a lock, and another ownerless process can join after the observation but
before statement commit. The bounded performance target is therefore to reduce
repeated lock churn while still holding the real directory-owned page-write
lock.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` acquires ownerless page-write
  locks in `mtr_t::ownerless_page_write_enter()` and normally releases
  MTR-scoped pages from `ownerless_page_write_leave_low()`.
- The same file uses `ownerless_page_write_holds_for_transaction()` only for
  non-system, non-undo persistent pages. Those pages are recorded in
  `trx_t::mylite_ownerless_modified_pages` and are published at commit by
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()`.
- Native-support pages such as undo and transaction-system pages are not safe
  to record in the transaction-deferred modified-page vectors, because
  `mariadb/storage/innobase/trx/trx0trx.cc` treats those vectors as
  commit-time page-version publication input and as visible-fast proof state.
- `ownerless_page_write_can_elide_native_support_page()` already identifies
  native-support pages whose page-version publish would only be elided, and it
  refuses pages that are the active rollback-segment/undo history-proof pair.
- `mylite_ownerless_innodb_can_skip_external_page_refresh()` proves the current
  process is in the single-owner fast-path shape, but it is only an observation.
  It can gate whether longer lock holding is worth doing; it cannot replace the
  shared page-write lock.
- `mylite_ownerless_innodb_lock_release_transaction_page_writes()` releases all
  page-write locks owned by the transaction's ownerless page-write identity and
  is already called by transaction cleanup, rollback/forget paths, and
  page-write deadlock retry paths.

## Design

Add a separate transaction-local native-support page-write vector and
membership set on `trx_t`. This vector records native-support page-write locks
that were actually acquired and should stay held until transaction cleanup. It
is intentionally separate from the modified/dirty/page-image vectors, so it is
not considered transaction-deferred page publication state.

`mtr_t::ownerless_page_write_enter()` may hold a native-support page-write lock
beyond the MTR only when all of these are true:

- ownerless page-write hooks are active and transaction cleanup release is in
  use;
- the SQL statement is autocommit and eligible for the ownerless visible-fast
  path;
- the page is an in-file persistent native-support page that
  `ownerless_page_write_can_elide_native_support_page()` would elide;
- the page is not the active history-proof rollback-segment/undo pair; and
- the current runtime is in the single-owner fast-path shape.

When the page is already in the new transaction-local native-support vector,
the later MTR skips acquire/release work and relies on the existing shared
page-write lock. When the page is first acquired without waiting, the MTR
records it in the new vector instead of the MTR-local page list. If acquisition
waited, the path falls back to normal MTR-scoped release and refresh/boundary
behavior.

Transaction page-write release clears the native-support vector when it
releases the shared page-write locks. That prevents stale local membership
after deadlock retry or rollback/forget paths release transaction page-write
locks before ordinary transaction object cleanup.

Two page-write perf counters prove coverage:

- native-support page-write locks first held to transaction cleanup;
- later MTR hits on a native-support page already held by the transaction.

## Compatibility Impact

SQL results, isolation semantics, page-version WAL format, public API behavior,
and directory layout are unchanged. The change only extends the lifetime of
actual shared page-write locks for native-support pages that the existing
native-support rules already prove do not need per-MTR page-version records.

The slice does not claim SQL-level table-lock fault injection, broader
DDL/file-lifecycle recovery, broader active-reader pressure policy, broader
DDL/dictionary/space-allocation classes, or external MariaDB/RQG stress.

## Native Storage Impact

Native InnoDB page images remain in MariaDB format. User data/index/blob pages
continue to use the existing transaction-deferred page-version publication
path. History-proof rollback-segment and undo pages continue to publish the
required native-support proof records and are not converted into blind
native-support lock holds while the history proof is active.

## Build And Performance Impact

The implementation adds one lazily allocated transaction-local vector and one
optional membership set per transaction only after the optimized path is used.
It should reduce repeated ownerless page-write acquire/release work inside
autocommit visible-fast statements that touch the same elidable native-support
page multiple times.

## Test Plan

- Extend focused ownerless SQL coverage to assert native-support WAL elision
  still occurs while native-support page-write locks are held to transaction
  cleanup.
- Extend the visible-fast multi-row insert case to assert at least one
  transaction-held native-support page and at least one later same-statement
  hit.
- Expose the new counters through the embedded performance probe.
- Run focused ownerless native-support, visible-fast, and history-proof SQL
  selectors, ownerless primitive/hook coverage, a reduced production
  stats-enabled probe, format checks, and `git diff --check`.

## Acceptance Criteria

- Native-support held-lock tracking never feeds the modified/dirty
  transaction-deferred page vectors.
- The focused native-support WAL elision test still observes elided and
  published native-support page classes with no publish failures.
- The visible-fast multi-row insert test observes native-support held-lock
  reuse inside one statement.
- Transaction page-write release clears held native-support membership when it
  releases shared page-write locks.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- focused direct SQL cases:
  `test_ownerless_single_owner_native_support_page_wal_elision`,
  `test_ownerless_single_owner_multi_row_insert_visible_fast_path`,
  `test_ownerless_single_owner_history_wal_proof`,
  `test_ownerless_explicit_transaction_undo_wal_elision`, and top-level
  `commit-race`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-ownerless-innodb-lock-hooks|embedded-ownerless-product-hooks)$'
  --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-single-owner-native-support-page-wal-elision|ownerless-single-owner-multi-row-insert-visible-fast-path|ownerless-single-owner-history-wal-proof|ownerless-negative-proof|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test && ctest --preset ownerless-stress
  --output-on-failure`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

Reduced stats-enabled 2048-row production probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=20480
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The probe reported `42114` held native-support page-write locks and `84999`
already-held hits in the ownerless autocommit insert phase. The 2048-row bulk
phase reported `47` held native-support page-write locks and `36947`
already-held hits.

Reduced stats-off 2048-row production probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=20480
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The stats-off sample reported ordinary 2048-row bulk rows at
`125321.71 ops/s` and ownerless 2048-row bulk rows at `63087.14 ops/s`, ratio
`0.5034`. Remaining non-empty bulk statements reported ordinary rows at
`124781.62 ops/s` and ownerless rows at `59271.75 ops/s`, ratio `0.4750`.

## Risks

- The optimization intentionally does not hold through waited acquisitions,
  preserving the existing refresh and boundary behavior under contention.
- The single-owner check is used only as a performance gate. Correctness
  depends on the shared page-write lock remaining held until transaction
  cleanup.
- The slice addresses repeated native-support page-write lock churn, not the
  deeper native row insert or undo-record construction costs that still remain
  in the attribution data.
