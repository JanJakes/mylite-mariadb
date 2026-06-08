# Ownerless Undo Cache Reuse Profile

## Problem

Ownerless history-flush identity profiling shows the rollback-segment-space
flush is almost entirely fresh page identities: a 100-row stats-enabled
autocommit sample reported `281` ownerless history flush pages with `279`
unique identities and only `2` duplicates. That rules out repeated flushes of
the same native history pages as the next performance target.

MariaDB normally reduces undo-log churn by reusing one-page cached undo logs in
the selected rollback segment. The ownerless path currently bypasses that reuse
while hooks are enabled, which may explain why simple ownerless autocommit
inserts keep creating and flushing fresh undo/system pages. Before changing the
reuse policy, MyLite needs direct production-build evidence that the guarded
ownerless path is blocking otherwise eligible undo-cache reuse.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `trx_assign_rseg_low()` chooses persistent rollback segments in round-robin
  order and assigns `trx->rsegs.m_redo.rseg`.
- `trx_undo_assign()` and `trx_undo_assign_low<false>()` normally call
  `trx_undo_reuse_cached()` before creating a fresh undo log, but the current
  MyLite ownerless guard skips that reuse when
  `mylite_ownerless_innodb_lock_has_hooks()` is true.
- `trx_purge_add_undo_to_history()` only places a committed one-page undo log
  into `rseg->undo_cached` when the log is small enough and either ownerless
  hooks are not enabled or the transaction has no shared rw-trx registry
  element. Ordinary ownerless DML with a shared transaction element therefore
  remains eligible by size but is intentionally sent to purge instead of the
  cache.
- Ownerless purge/history code already refreshes the rollback-segment header,
  first history-list undo page, and space allocation metadata before mutating
  cross-process visible history-list state. That does not by itself prove
  cached undo reuse is safe across peers.

## Design

Add opt-in deep-performance counters only. Do not change undo-cache behavior.

`trx_undo_assign()` and persistent `trx_undo_assign_low<false>()` will count:

- persistent undo assignment calls,
- assignment calls that already have an undo log,
- cached-undo reuse attempts,
- cached-undo reuse hits,
- cached-undo reuse misses,
- ownerless cached-undo reuse skips,
- fresh undo-log create calls,
- fresh undo-log create successes.

`trx_purge_add_undo_to_history()` will count:

- history records whose undo log is size-eligible for caching,
- size-eligible history records blocked by the current ownerless guard,
- history records actually moved to `rseg->undo_cached`,
- history records marked `TRX_UNDO_TO_PURGE`.

`mylite_embedded_performance_probe` will emit raw deep-perf counters and
ownerless-autocommit summary keys. The summary will make the blocked-ownerless
ratio visible as `blocked_ownerless / cache_eligible`.

## Compatibility Impact

No SQL, C API, PHP API, native storage, recovery, or directory-layout behavior
changes. The slice only adds diagnostic counters under the existing
stats-enabled performance path.

## Native Storage Impact

No undo, redo, page, checkpoint, or rollback-segment format change. Cached undo
reuse remains disabled for ownerless DML under the existing guard.

## Binary Size Impact

No new dependency. The embedded MariaDB archive gains a small number of deep
perf counter increments in stats-enabled transaction/undo paths.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded performance
  probe.
- Run a reduced stats-enabled production embedded performance probe and verify
  the undo-cache keys are present.
- Run a stats-off production embedded performance probe to ensure normal
  timings still run without stats.
- Run focused production ownerless commit/read/reclaim selectors.
- Run production format and whitespace checks.

## Acceptance Criteria

- Stats-enabled ownerless autocommit output reports undo assignment, cache
  attempt/hit/miss, ownerless cache-skip, history cache-eligible,
  history-cache-blocked, cached, and to-purge counters.
- The summary output exposes whether the current ownerless guard is blocking
  otherwise eligible cached undo reuse.
- No undo-cache behavior changes are made in this slice.
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
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported ownerless
  autocommit at `479.43 ops/s` versus ordinary autocommit at
  `1996.58 ops/s`.
- The same stats-enabled ownerless autocommit sample reported `1.000` undo
  assignment call per insert, `1.000` ownerless cached-undo reuse skip per
  insert, `0.000` cache reuse attempts per insert, `1.000` fresh undo-log
  create call per insert, `1.000` size-eligible history record per insert,
  `1.000` ownerless-blocked eligible history record per insert, `0.000`
  cached history records per insert, and a blocked-ownerless ratio of
  `1.0000`.
- Raw counters for the explicit transaction insert phase showed the contrast:
  `100` undo assignment calls, `99` existing-log assignments, `1` ownerless
  cached-undo reuse skip, and `1` fresh undo-log create, confirming the
  autocommit result is not a counter-reset artifact.
- A default stats-off production probe passed and reported ordinary autocommit
  at `2127.06 ops/s`, ownerless autocommit at `268.06 ops/s`, and an
  ownerless autocommit ratio of `0.1260`.
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

- If ownerless DML is mostly blocked from cached undo reuse, a follow-up
  correctness slice must prove that reusing cached undo logs cannot expose stale
  history-list links, stale rollback-segment header state, or unsafe page
  ownership across live peers.
- If the counters show little blocked reuse, the next performance target should
  stay on page-version/native-support publication or row-insert internals.
