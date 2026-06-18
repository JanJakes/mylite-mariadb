# Ownerless Read Path Attribution

## Problem

WordPress PHPUnit timing is now split enough to show that the long CI wall time
is dominated by non-isolated test execution, not by the build steps. The
embedded production probe reports ownerless versus ordinary `SELECT 1` ratios,
but it does not expose which ownerless statement stages account for read-path
overhead. That leaves direct and prepared read optimization decisions dependent
on local ad hoc profiling.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB embedded execution remains the timed engine boundary. MyLite direct
  SQL ultimately dispatches through `mysql_query()` and prepared SQL through
  `mysql_stmt_execute()`, matching the embedded interface documented in
  `docs/architecture/mariadb-foundation.md`.
- MyLite direct ownerless statements run through
  `exec_result_impl()` in `packages/libmylite/src/database.cc`.
- MyLite prepared ownerless statements run through `mylite_step()` in
  `packages/libmylite/src/database.cc`, where existing
  `OWNERLESS_DATABASE_PERF_PREPARED_STEP_*` counters already split pressure,
  statement locking, refresh, snapshot pinning, native execution, and reclaim.
- Ownerless page-version read hooks record
  `OWNERLESS_DATABASE_PERF_PAGE_READ_*` counters, but the production probe only
  enables ownerless database stats for write attribution.

## Design

Extend `mylite_embedded_performance_probe` so the ownerless direct and prepared
`SELECT 1` sections can run with ownerless database perf stats enabled when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

The probe should emit compact read-path summaries after each ownerless read
section:

- direct select ownerless database stage totals;
- prepared select ownerless database stage totals;
- page-version read hook totals, even when they are zero;
- prepared-step per-iteration averages for the already-instrumented stages.

This is instrumentation only. It must not change statement dispatch, ownerless
refresh policy, SQL results, native storage, page-version WAL format, or CI
pass/fail thresholds.

## Scope And Non-Goals

In scope:

- production embedded performance probe output;
- CI-visible ownerless direct/prepared read-path attribution;
- compatibility documentation for the new timing evidence.

Out of scope:

- changing ownerless read visibility policy;
- changing MariaDB or InnoDB execution;
- adding new dependencies;
- comparing against an external MariaDB server.

## Compatibility And Storage Impact

No SQL, C API, PHP/mysqli, native storage, directory-layout, wire-protocol, or
metadata behavior changes. The slice only reports existing in-process timing
counters during the production probe.

## Test Plan

- Build `mylite_embedded_performance_probe` in `php-embedded-prod`.
- Run the probe with `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and verify
  the new direct/prepared read summary lines are present.
- Run the focused ownerless visible-fast selector to ensure the previous write
  optimization remains intact.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output includes ownerless direct and prepared read-path attribution
  lines under production builds.
- The new counters are zero-safe and do not fail when a read path avoids
  page-version hooks.
- Existing performance probe summaries and write attribution remain present.
- Documentation records that these are diagnostic counters, not compatibility
  behavior.

## Risks

Perf stats collection adds overhead while enabled, so the default probe should
continue to leave these counters off unless the existing attribution flag is
enabled. The CI attribution step already uses that flag and is meant for
diagnostics rather than headline throughput.

## Implementation Evidence

The implementation reuses existing MyLite counters:

- `mylite_exec_result_perf_*` for direct SQL timing;
- `mylite_ownerless_database_*` for prepared-step and page-version read-hook
  timing.

The production probe resets and enables those counters only around the
ownerless direct/prepared `SELECT 1` sections when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, then disables them before the
write attribution sections so write counters remain isolated.

A reduced local `php-embedded-prod` attribution run on 2026-06-18 used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=25 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It emitted the expected direct and prepared read summary keys. The sample
reported zero page-version read hooks and zero WAL-scan time for both
tableless read shapes; direct `SELECT 1` spent `0.220 ms/select` in
`mysql_query()`, while prepared `SELECT 1` spent `0.206 ms/select` in the
prepared-step envelope and `0.202 ms/select` inside native MariaDB execute.
