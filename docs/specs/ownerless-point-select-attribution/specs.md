# Ownerless Point Select Attribution

## Problem

The first ownerless read-path attribution pass proved that tableless `SELECT 1`
does not exercise ownerless page-version reads, so it cannot explain the slow
WordPress PHPUnit chunks that run real schema/table queries. The production
probe needs a real InnoDB indexed point-select shape with the same ownerless
stage counters before optimization work targets read overhead.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Direct SQL still reaches MariaDB through `mysql_query()` from
  `exec_result_impl()` in `packages/libmylite/src/database.cc`.
- Prepared SQL still reaches MariaDB through `mysql_stmt_execute()` from
  `mylite_step()` in `packages/libmylite/src/database.cc`.
- Existing `mylite_ownerless_database_*` counters already split prepared
  ownerless pressure, statement lock, refresh, snapshot pin, native execution,
  reclaim, and page-version read-hook time.
- Existing `mylite_exec_result_perf_*` counters split direct exec result time
  into native query, store-result, schema, status, and native-control stages.

## Design

Extend `mylite_embedded_performance_probe` with an indexed InnoDB point-select
table and four timed read sections:

- ordinary direct `SELECT value FROM app.<table> WHERE id = 1`;
- ordinary prepared `SELECT value FROM app.<table> WHERE id = ?`;
- ownerless direct point select;
- ownerless prepared point select.

When `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, reset and enable the existing
read counters around the ownerless point-select sections and emit compact
per-select summaries for direct exec stages, prepared step stages, and
page-version read-hook counts/time.

## Scope And Non-Goals

In scope:

- production embedded performance probe output;
- ordinary/ownerless direct and prepared point-select rates;
- ownerless point-select read-stage attribution under the existing stats flag.

Out of scope:

- changing ownerless read refresh policy;
- changing MariaDB/InnoDB execution;
- changing PHP/mysqli or WordPress PHPUnit harness behavior;
- adding pass/fail thresholds for the new timing metrics.

## Compatibility And Storage Impact

No SQL behavior, C API behavior, directory layout, native storage format, or
ownerless coordination semantics change. The slice creates and reads a temporary
probe table inside the probe database directory, then removes the whole probe
directory as before.

## Test Plan

- Build `mylite_embedded_performance_probe` in `php-embedded-prod`.
- Run a reduced production probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and verify the point-select
  summary keys are present.
- Run focused ownerless selectors for tableless read fast path and
  visible-fast insert safety.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output includes ordinary/ownerless direct and prepared point-select
  rates.
- Stats-enabled probe output includes ownerless direct/prepared point-select
  per-select summaries for direct exec, prepared step, and page-version reads.
- The probe remains stats-off by default for headline throughput.
- Documentation records that this is diagnostic instrumentation, not a
  compatibility claim.

## Risks

The point-select shape is still a small hot-buffer production probe. It is useful
for distinguishing tableless reads from real InnoDB table reads, but it is not a
substitute for full WordPress PHPUnit timing or external MariaDB/RQG stress.

## Implementation Evidence

A reduced local `php-embedded-prod` attribution run on 2026-06-18 used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=25 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It emitted the expected ordinary/ownerless direct and prepared point-select
rates and ownerless point-select summary keys. In that sample, ownerless direct
and prepared point-select ratios were `0.5085` and `0.4280`, page-version read
hooks remained zero, direct `mysql_query()` cost was `0.447 ms/select`, and
prepared point-select cost was `0.497 ms/select`, split mostly between
`0.443 ms/select` native MariaDB execute and `0.038 ms/select` ownerless
refresh.
