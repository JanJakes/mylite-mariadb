# Ownerless History Flush Identity Profile

## Problem

Ownerless autocommit profiling now shows the rollback-segment-space native
history flush is a major remaining per-insert cost, and the page-type profile
shows a small set of undo-log and system pages make up the flushed pages. It
does not show whether those pages are mostly fresh pages or repeated flushes of
the same rollback-segment-space pages across the measured phase. Without page
identity evidence, the next performance slice would be guessing between undo
page allocation churn, repeated rollback-segment header/system-page writes, or
a broader native redo/checkpoint reconciliation change.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `trx_t::write_serialisation_history()` records the ownerless
  rollback-segment-space flush after the history MTR commits and before the
  ownerless history page-write lock is released.
- `buf_flush_wait_space_flushed()` scopes ownerless flush profiling to the
  current thread while it loops on `buf_flush_list_space()` until the target
  tablespace has no dirty page below the requested LSN.
- `buf_flush_list_space()` walks the global flush list, filters by
  `bpage->id().space()`, locks the target page, and calls
  `bpage->flush(space)`. The existing page-type profile increments only after
  that flush reports success.
- The page identity needed for attribution is available before the flush from
  `space->id`, `bpage->id().page_no()`, and the `FIL_PAGE_TYPE` field read with
  `fil_page_get_type()`.

## Design

Keep InnoDB and ownerless durability behavior unchanged. Extend the opt-in
ownerless InnoDB deep-performance stats with a bounded identity table used only
while the existing ownerless history-flush page-type profile is active.

Each successful target-space flush records a fingerprint of:

- tablespace id,
- page number,
- page type.

The bounded table classifies every successful flush as one of:

- unique page identity,
- duplicate page identity,
- table overflow.

Duplicate flushes are also classified into the same page-type buckets used by
the page-type profile: undo-log, index, FSP header, XDES, inode, allocated,
system, transaction-system, or other. The table is reset through
`mylite_ownerless_innodb_deep_reset_perf_stats()`, which is already used by the
production performance probe before each measured stats-enabled insert phase.

`mylite_embedded_performance_probe` emits raw deep-perf identity counters and
ownerless-autocommit summary keys. In stats-enabled mode it fails if
`unique + duplicate + overflow` differs from the existing ownerless history
flush page total.

## Compatibility Impact

No SQL, C API, PHP API, directory layout, native storage, or ownerless
coordination behavior changes. This is internal opt-in performance
instrumentation for production-build timing analysis.

## Native Storage Impact

No native page, redo, undo, checkpoint, or tablespace format change. The
history handoff still waits for rollback-segment-space native flush proof
before releasing the ownerless history page-write lock.

## Binary Size Impact

No new dependency. The embedded MariaDB archive gains a small bounded identity
table and additional deep-perf counters used only in the stats-enabled
performance path.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded performance
  probe.
- Run a reduced stats-enabled production embedded performance probe and verify
  the raw and summary identity keys are present.
- Run focused production ownerless commit and live-reclaim selectors to verify
  ownerless behavior is unchanged.
- Run the focused ownerless hook crash selectors that exercise page-visible
  publish and checkpoint boundaries.
- Run production format and whitespace checks.

## Acceptance Criteria

- Stats-enabled ownerless autocommit probes report unique, duplicate, and
  overflow identity counts for ownerless history flushes.
- The probe fails if identity accounting does not add up to the existing
  ownerless history flush page total.
- Duplicate page identities are classified by page type for rollback-segment
  performance diagnosis.
- Existing focused production ownerless correctness coverage still passes.

## Verification Results

Local verification on 2026-06-09 used production embedded builds:

- `tools/mariadb-embedded-build build` passed and rebuilt the MariaDB embedded
  archive with `CMAKE_BUILD_TYPE=MinSizeRel`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- A reduced stats-enabled production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported `281` ownerless
  history flush pages, `281` identity-classified pages, `279` unique page
  identities, `2` duplicate page identities, and `0` table-overflow pages. The
  duplicate identities split into `1` undo-log page and `1` system page. The
  summary identity ratio was `1.0000`, duplicate-type ratio was `1.0000`, and
  duplicate page ratio was `0.0071`.
- The same stats-enabled sample reported ordinary autocommit at
  `1625.57 ops/s`, ownerless autocommit at `492.00 ops/s`, and an ownerless
  autocommit ratio of `0.3027`.
- A default stats-off production probe passed and reported ordinary autocommit
  at `1899.93 ops/s`, ownerless autocommit at `359.53 ops/s`, and an
  ownerless autocommit ratio of `0.1892`.
- Focused production ownerless SQL selectors passed: `commit-race`,
  `prepared-committed-read`, `local-write-first-read`, and `live-reclaim`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and hook selectors
  `visible-publish-crash` and `visible-checkpoint-crash` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- This slice does not claim a throughput improvement. It decides whether the
  next optimization should target repeated native page flushes or fresh
  rollback-segment-space churn.
- The identity table is intentionally bounded. Overflow is counted explicitly
  so larger future samples can distinguish table pressure from true unique-page
  pressure.
- The follow-up ownerless undo-cache reuse profile measures whether fresh
  rollback-segment-space churn is caused by the current guard that bypasses
  MariaDB's one-page cached undo reuse in ownerless mode.
