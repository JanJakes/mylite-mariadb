# Ownerless Held Native-Support Publish Skip

## Problem

Ownerless non-empty bulk autocommit statements still spend measurable time in
the no-dirty mini-transaction commit-log page-publication path. The previous
bulk phase split showed that later 100-row statements still execute many
commit-log/no-dirty publication checks even after native-support page-write
locks are held and reused inside the statement.

The existing stats-off native-support fast skip avoids calling the full publish
helper only after inspecting the current page image and page type. For pages
already recorded as held native-support page-write locks in the active
transaction, the transaction-local held-page proof is stronger and cheaper:
the page has already passed the existing native-support elision predicate when
the shared page-write lock was acquired and kept until transaction cleanup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` owns the ownerless MTR commit-log,
  page-write acquisition, native-support elision, and page-publication paths.
- `ownerless_page_write_can_hold_native_support_page()` records a page in
  `trx_t::mylite_ownerless_native_support_page_write_pages` only after the
  same native-support elision predicate accepts the page and external page
  refresh can be skipped.
- `ownerless_page_write_can_fast_skip_elided_native_support_publish()` is a
  stats-off production dispatch cleanup, but it still re-reads the page image,
  page LSN, and page type before proving the publish helper would elide.
- `ownerless_page_write_history_proof_roles()` identifies rollback-segment and
  undo pages that must still publish proof WAL; those pages must not be skipped
  merely because they are native-support pages.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has a
  focused `single-owner-native-support-page-wal-elision` selector proving
  native-support elision and held native-support page-write reuse.

## Design

Add a narrower publish-dispatch fast path for pages already held in
`mylite_ownerless_native_support_page_write_pages`.

The skip is allowed only when:

- page-publish statistics are disabled, so detailed native-support elision
  counters keep their existing meaning;
- the mini-transaction has a nonzero commit LSN;
- the page is an in-file, non-temporary page;
- the active transaction is not read-only or a dictionary operation;
- the page has no active rollback-segment or undo history-proof role; and
- the active transaction's held native-support page-write list contains the
  page.

When page-write perf stats are enabled, the path increments a new
`native_support_transaction_publish_skipped` counter before returning. The
counter lets perf-only probes prove the production skip without enabling
page-publish stats, which intentionally keep the full publication/elision
helper active.

## Compatibility Impact

No SQL, C API, metadata, or wire-protocol behavior changes. The slice changes
only ownerless InnoDB internal dispatch for a page-publication call that would
already have been elided by the existing native-support rules. It does not skip
native undo records, history-proof WAL, transaction-deferred user page
publication, redo completion, page-write locking, or transaction cleanup.

## Directory And Lifecycle Impact

No directory layout changes. The existing transaction cleanup path still owns
the shared native-support page-write lock release and clears the
transaction-local held-page list.

## Native Storage Impact

Native InnoDB page images, redo, undo, and buffer-pool latches remain owned by
the existing MariaDB paths. The skip only avoids a redundant ownerless
page-publication helper dispatch for pages that are already proven
native-support and transaction-held.

## Build And Performance Impact

The production stats-off path does less per-page work for repeated held
native-support page modifications inside visible-fast autocommit statements.
Diagnostics remain available: page-publish stats preserve full elision
accounting, while page-write perf stats can count held native-support publish
skips.

## Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the focused `single-owner-native-support-page-wal-elision` selector.
- Run ownerless primitive coverage and reduced production embedded performance
  probes with page-write-only attribution, default stats-off timing, and
  page-publish phase-split attribution.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Held native-support pages can skip redundant publish dispatch when
  page-publish stats are disabled.
- Page-publish stats-enabled coverage still exercises the full helper and keeps
  native-support elision/history-proof counters meaningful.
- Focused SQL coverage proves the new page-write perf skip counter becomes
  positive on a multi-row visible-fast insert while page-publish stats are off.
- Existing native-support WAL elision, history-proof publication, visible-fast
  commit publication, reopen visibility, and forced `.shm` rebuild coverage
  continue to pass.

## Risks And Unresolved Questions

- This is a bounded hot-path cleanup. It does not remove the necessary
  rollback-segment and undo history-proof WAL records, and it does not resolve
  the broader native redo/checkpoint reconciliation or DDL/file-lifecycle
  recovery gaps.
- The exact throughput improvement is expected to be modest and workload
  dependent because row-level native undo and InnoDB insert work remain larger
  contributors for non-empty bulk statements.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-native-support-page-wal-elision$'
  --output-on-failure`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(ownerless-single-owner-history-wal-proof|ownerless-single-owner-multi-row-insert-visible-fast-path|ownerless-single-owner-page-write-refresh-skip)$'
  --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-single-owner-history-wal-proof|ownerless-single-owner-native-support-page-wal-elision|ownerless-single-owner-multi-row-insert-visible-fast-path|ownerless-history-proof-publish-failure-fallback)$'
  --output-on-failure`
- `ctest --preset ownerless-stress --output-on-failure`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

Reduced page-write-only production probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The 1000-row sample reported `20` held native-support page-write locks,
`1822` held native-support hits, and `901` held native-support publish skips in
the ownerless bulk phase. Bulk no-dirty-loop time was `2.635 ms`, with
`1.810 ms` in no-dirty page-publish attribution and `2.145 ms` total
commit-log publish attribution.

Reduced default stats-off production probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The stats-off sample reported ownerless bulk remaining rows at `40409.00
rows/s` versus ordinary at `129754.21 rows/s`, ratio `0.3114`. Warm open/close
was `121.948 ms` for ownerless versus `147.409 ms` ordinary in that run, and
active runtime reconnect was `0.985 ms` ownerless versus `0.843 ms` ordinary.

Reduced page-publish stats phase-split probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=200
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The remaining-statement phase kept detailed page-publish accounting active:
`2.000` published native-support pages, `100.000` elided native-support pages,
`3.000` page-publish hook calls, and `0.000` held native-support publish skips
per statement. The one-statement guard emitted `0.000` for remaining page
versions, page-publish hook calls, commit-log calls, and the new remaining
publish-skip row.
