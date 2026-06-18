# Ownerless Refresh Attribution

## Problem

The ownerless read-hook attribution slice showed that MDL, transaction, and
read-view hook callback bodies are not the main cost in ownerless InnoDB point
selects. The remaining prepared point-select overhead still includes a visible
`refresh_ownerless_external_pages_before_statement()` stage. That refresh path
owns dictionary generation checks, shared redo/process snapshots, page-version
pin decisions, clean-page refresh, native page visibility, and current read-view
state, so it needs finer attribution before any read fast path can be justified.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Direct ownerless SQL reaches `refresh_ownerless_external_pages_before_statement()`
  from `exec_result_impl()` before `mysql_query()`.
- Prepared ownerless SQL reaches the same refresh function from `mylite_step()`
  before `mysql_stmt_execute()`, and currently reports that whole function only
  as `PREPARED_STEP_REFRESH_NS`.
- The refresh function calls `refresh_ownerless_dictionary_before_statement()`,
  snapshots ownerless redo/process/transaction state from the shared mapping,
  may snapshot active page-version pins, may register or reuse a handle pin,
  may close the current native read view, may refresh clean native pages, and
  may enable external page visibility for the upcoming MariaDB statement.

## Design

Append diagnostic counters to the existing `mylite_ownerless_database_*` counter
block without reordering existing indices. The new counters split
`refresh_ownerless_external_pages_before_statement()` into:

- call count and total elapsed time;
- dictionary generation check time;
- shared redo/process/transaction snapshot time;
- active page-pin snapshot time;
- baseline page-version pin registration time;
- local transaction horizon advancement time;
- current read-view close time;
- native flush time;
- external clean-page refresh time;
- handle page-version pin registration/reuse time;
- forced clean-page refresh time;
- visibility push/space-header refresh time;
- external page visibility enable time;
- local-native-current-read and page-version-enabled decisions.

The production embedded performance probe already resets/enables ownerless
database counters around the tableless and point-select read windows when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. Mirror the appended counters in
the probe and emit compact per-select refresh summaries for ownerless
direct/prepared tableless reads and ownerless direct/prepared point selects.

## Scope And Non-Goals

In scope:

- diagnostic counters in first-party MyLite ownerless refresh code;
- production probe summaries under the existing attribution flag;
- documentation that the slice is evidence-gathering for read-path performance.

Out of scope:

- changing dictionary refresh, page-version pin, read-view, or native visibility
  policy;
- changing SQL behavior, public C API behavior, durable directory layout, or
  native storage format;
- adding pass/fail timing thresholds for refresh sub-stages.

## Compatibility And Storage Impact

This slice is diagnostic-only. It does not change MySQL/MariaDB SQL behavior,
public MyLite API behavior, ownerless coordination semantics, or files in the
MyLite database directory.

## Test Plan

- Build `mylite_embedded_performance_probe` in `php-embedded-prod`.
- Run a reduced production probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and verify tableless and
  point-select refresh summary keys are present.
- Build `mylite_ownerless_cross_process_sql_test`.
- Run focused ownerless selectors for tableless read fast path and
  visible-fast insert safety.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Refresh counters are appended without invalidating existing database perf
  counter indices.
- Stats-enabled probe output reports per-select refresh sub-stage summaries for
  ownerless tableless and point-select reads.
- No ownerless correctness policy changes are made.

## Implementation Evidence

A reduced local `php-embedded-prod` attribution run on 2026-06-18 used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=100 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The run emitted the new tableless and point-select refresh summary keys.
Tableless direct and prepared `SELECT 1` reported refresh calls but no shared
snapshot, page-version pin, clean-page refresh, visibility, or native flush
work. Ownerless direct point selects reported `0.036 ms/select` total refresh
time, with `0.035 ms/select` in the shared redo/process/transaction snapshot,
`1.000` current-read-view close attempt per select, zero page-version reads,
zero handle-pin registrations, zero clean-page refreshes, and
`1.000` local-native-current-read decisions per select. Ownerless prepared point
selects reported `0.035 ms/select` total refresh time, with `0.034 ms/select`
in the shared snapshot, zero page-version reads, zero handle-pin registrations,
zero clean-page refreshes, and `1.000` local-native-current-read decisions per
select.

The sample points the next read-path optimization toward reducing repeated
shared snapshot work in proven local-native-current-read hot loops, not toward
page-version WAL lookup, handle-pin registration, or clean-page refresh.

## Risks

Refresh-stage attribution can identify the expensive branch, but it does not
prove a safe optimization by itself. Any later fast path must separately prove
that peer joins, peer DDL, active page-version pins, retained clean pages, and
native read-view lifetime remain safe.
