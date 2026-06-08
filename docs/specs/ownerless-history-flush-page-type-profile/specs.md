# Ownerless History Flush Page Type Profile

## Problem

Production ownerless autocommit profiling now shows that the remaining
write-history handoff cost is concentrated in the rollback-segment-space
native flush. The current probe reports the elapsed flush time and total pages
flushed, but not which page classes make up those pages. Without page-type
evidence, the next performance slice would be guessing between rollback-segment
selection, undo-log page churn, allocation/header churn, or a broader native
redo/checkpoint reconciliation change.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `trx_t::write_serialisation_history()` runs the ownerless
  rollback-segment-space target-LSN flush after the history MTR commits and
  before releasing the ownerless history page-write lock.
- `buf_flush_wait_space_flushed()` calls `buf_flush_list_space()` until the
  target tablespace has no older dirty page below the requested LSN.
- `buf_flush_list_space()` walks the global flush list, filters by
  `bpage->id().space()`, takes the page lock, and then calls
  `bpage->flush(space)` for pages in that one tablespace.
- InnoDB page type is stored in the `FIL_PAGE_TYPE` field and read with
  `fil_page_get_type()`. The relevant expected buckets for undo tablespaces
  are `FIL_PAGE_UNDO_LOG`, `FIL_PAGE_TYPE_FSP_HDR`, `FIL_PAGE_TYPE_XDES`,
  `FIL_PAGE_INODE`, `FIL_PAGE_TYPE_SYS`, `FIL_PAGE_TYPE_TRX_SYS`, and
  occasional allocated or index/other pages.

## Design

Keep the flush wait and ownerless lock release semantics unchanged. Add
opt-in deep-performance counters in the existing ownerless InnoDB deep-perf
array and increment them only after `bpage->flush(space)` reports that a page
was actually flushed.

The page-type buckets are:

- undo-log pages,
- index pages,
- FSP header pages,
- XDES pages,
- inode pages,
- allocated pages,
- system pages,
- transaction-system pages,
- other pages.

`mylite_embedded_performance_probe` emits both raw
`*_trx_commit_persist_write_history_ownerless_flush_*_pages` counters and
compact per-insert `mylite_perf_summary_*` keys for the ownerless autocommit
phase. A follow-up probe guard sums all page-type buckets and fails the
stats-enabled production probe if the sum differs from the existing ownerless
history flush page total, so future performance comparisons do not silently use
partial attribution data.

## Compatibility Impact

No SQL, C API, PHP API, directory layout, native storage, or ownerless
coordination behavior changes. This is internal opt-in performance
instrumentation.

## Native Storage Impact

No native page, redo, undo, or checkpoint format change. The history handoff
still waits for the rollback-segment tablespace to reach the history MTR LSN
before the ownerless page-write lock is released.

## Binary Size Impact

No new dependency. The embedded MariaDB archive gains nine additional deep
perf counters and one page-type classification branch in an existing
stats-enabled performance-probe path.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded performance
  probe.
- Run a reduced stats-enabled production embedded performance probe and verify
  the raw and summary page-type keys are present.
- Run focused production ownerless commit selectors to verify the ownerless
  history handoff behavior is unchanged.
- Run production format and whitespace checks.

## Acceptance Criteria

- Page-type counters add up to the existing ownerless history flush page count
  for stats-enabled samples, excluding pages flushed by concurrent background
  work outside this wait.
- The performance probe prints raw and per-insert page-type summaries.
- Stats-enabled ownerless attribution probes report and enforce the
  page-type-bucket sum, per-insert bucket sum, and bucket-sum ratio.
- Focused production ownerless correctness coverage still passes.

## Verification Results

Local verification on 2026-06-08 used production embedded builds:

- `tools/mariadb-embedded-build build` passed and rebuilt the MariaDB embedded
  archive with `CMAKE_BUILD_TYPE=MinSizeRel`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- A reduced stats-enabled production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported `281` total
  ownerless history flush pages, split into `181` undo-log pages, `100`
  `FIL_PAGE_TYPE_SYS` pages, and zero index, FSP header, XDES, inode,
  allocated, `FIL_PAGE_TYPE_TRX_SYS`, or other pages. The summary keys reported
  `2.810` pages/insert, `1.810` undo-log pages/insert, and `1.000` SYS
  page/insert.
- A stats-off production performance probe passed. In that sample ordinary
  autocommit measured `1930.54 ops/s`, ownerless autocommit measured
  `500.66 ops/s`, and the ownerless autocommit ratio was `0.2593`.
- Focused production ownerless SQL selectors passed:
  `prepared-committed-read`, `local-write-first-read`, `commit-race`, and
  `live-reclaim`.
- Focused production ownerless CTests passed:
  `libmylite.embedded-ownerless-innodb-lock-hooks`,
  `libmylite.ownerless-primitives`, and the single-owner foreground reclaim,
  page-write refresh skip, external refresh skip, and peer-history selectors.
- `cmake --build --preset format-check-prod` passed.
- `cmake --build --preset tidy-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- This slice does not claim a throughput win. It identifies whether the next
  bounded optimization should target undo-log pages, rollback-segment metadata,
  allocation metadata, or rollback-segment locality.
- A follow-up identity profile distinguishes fresh page churn from repeated
  flushes of the same rollback-segment-space pages by counting unique,
  duplicate, and overflow identity classifications for the same successful
  native history flushes.
- The counters are opt-in and only intended for performance-probe diagnosis;
  they are not a public metrics ABI.
