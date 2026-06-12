# Ownerless Bulk Insert Performance Probe

## Problem

The ownerless write path now admits pure multi-row `INSERT ... VALUES` row
lists to the visible fast path, but the production embedded performance probe
still reports only repeated single-row prepared inserts for autocommit write
throughput. CI therefore cannot distinguish single-row per-commit overhead from
bulk row-list behavior, and future work on page-version append volume or
history-proof publication would lack a stable bulk-insert timing signal.

## Source Findings

- `packages/libmylite/tests/embedded_performance_probe.c` already owns the
  production `mylite_perf_*` output consumed by CI. It measures open/close,
  direct and prepared select, transactional insert, and single-row autocommit
  insert for ordinary and ownerless opens.
- The same probe has opt-in ownerless attribution under
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, including commit visibility,
  page publish, page log append, and deep InnoDB counters.
- `.github/workflows/ci.yml` already runs the probe twice from
  `php-embedded-prod`: once stats-off for throughput and once stats-on for
  reduced ownerless attribution. Adding parseable keys to the same binary keeps
  timing visible without a new CI step.

## Design

Add a direct multi-row autocommit insert phase to the embedded performance
probe. The phase uses the existing `MYLITE_PERF_INSERT_ITERATIONS` value as the
row count and a new `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT` setting as
the row-list size, defaulting to `4`. The output reports:

- configured rows per statement;
- ordinary and ownerless bulk row throughput;
- ordinary and ownerless bulk statement throughput;
- ownerless/ordinary bulk row-throughput ratio;
- bulk-specific ownerless raw attribution blocks when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`;
- compact bulk summary keys for page versions, page-log appends,
  native-support publication, and commit-visibility choices per row and per
  statement.

The existing single-row prepared autocommit metrics remain unchanged so branch
history stays comparable.

## Scope And Non-Goals

In scope:

- Production probe timing visibility for pure direct
  `INSERT ... VALUES (...), (...)` row lists.
- Docs that explain the new metric keys and how to interpret them.
- Verification with stats-off and stats-on reduced production probes.

Out of scope:

- Changing runtime SQL execution, ownerless page publication, history proof,
  CI build presets, WordPress timing, or PHPUnit harness behavior.
- Adding timing thresholds for bulk inserts. CI should publish numbers first;
  gates can come later once branch/main variance is understood.

## Compatibility Impact

No SQL, C API, PHP API, storage format, directory layout, or runtime behavior
changes. The probe executes already-supported SQL against temporary probe-owned
tables.

## Directory And Lifecycle Impact

The probe continues to create and remove one temporary MyLite database
directory under `TMPDIR` or `/tmp`. The new bulk table is dropped and recreated
inside the same temporary `app` schema as the existing insert phases.

## Native Storage Impact

No native storage behavior changes. The ownerless stats-on run will show how
many page-version records and native-support pages the bulk insert phase
publishes per row and per statement.

## Build And Performance Impact

The performance probe runs one additional ordinary bulk insert phase and one
additional ownerless bulk insert phase. With CI's current reduced attribution
settings, the default row count is `100` and the default row-list size is `4`,
so each phase executes `25` direct insert statements. Stats-off default probe
runtime increases modestly while producing visible bulk row and statement
timings.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced stats-off production probe and verify the new bulk keys print.
- Run a reduced stats-on production probe and verify ownerless bulk raw and
  summary attribution keys print.
- Run production-build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- `mylite_perf_bulk_insert_rows_per_statement` is printed.
- Ordinary and ownerless bulk row and statement throughput keys are printed.
- Stats-enabled runs print ownerless bulk attribution under a bulk-specific
  prefix and compact per-row/per-statement summary keys.
- Existing single-row insert timing keys remain present.
- No runtime library behavior changes outside the performance probe.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- Reduced stats-off production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=5
  MYLITE_PERF_INSERT_ITERATIONS=12
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=3
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  printed `mylite_perf_bulk_insert_rows_per_statement=3`,
  `mylite_perf_bulk_insert_statements=4`,
  ordinary bulk row throughput `6121.22 ops/s`, ownerless bulk row throughput
  `1004.11 ops/s`, and ownerless/ordinary bulk row ratio `0.1640`.
- Reduced stats-on production probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` printed bulk-specific ownerless
  raw attribution under `mylite_perf_ownerless_insert_autocommit_bulk_*`,
  including `commit_visibility_fast=4`, `commit_visibility_flush=0`,
  `page_publish_published=20`, and `page_log_append_calls=22`.
- The same stats-on run printed compact bulk summaries:
  `page_versions_per_row=1.667`, `page_versions_per_statement=5.000`,
  `page_log_append_calls_per_row=1.833`,
  `page_log_append_calls_per_statement=5.500`,
  `native_support_published_pages_per_row=0.667`,
  `native_support_elided_pages_per_row=2.000`, and
  `commit_visibility_fast_per_statement=1.000`.
- Existing single-row keys remained present in the stats-on run, including
  `mylite_perf_ordinary_insert_autocommit_ops_per_second=2242.44`,
  `mylite_perf_ownerless_insert_autocommit_ops_per_second=870.81`,
  `mylite_perf_summary_ownerless_autocommit_page_versions_per_insert=3.000`,
  and `mylite_perf_summary_ownerless_insert_autocommit_ratio=0.3883`.
- Production guards passed:
  `tools/check-ci-production-builds`,
  `tools/require-cmake-release-build build/php-embedded-prod`, and
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu:build/php-embedded-prod/packages/libmylite:build/mariadb-embedded/lib
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- Direct multi-row SQL includes parser/rendering cost and is not identical to
  prepared single-row execution. That is intentional: it measures the SQL shape
  whose ownerless fast-path boundary was just expanded.
- The next true optimization target remains reducing page-version append volume
  and history-proof/native-support page publication. This slice makes that
  target easier to track in CI but does not solve it.
