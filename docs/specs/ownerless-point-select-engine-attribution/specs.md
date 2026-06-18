# Ownerless Point-Select Engine Attribution

## Problem

After the single-owner refresh-snapshot fast path, stats-off production samples
show ownerless tableless prepared reads close to ordinary throughput, while
real InnoDB primary-key point selects remain slower, especially prepared point
selects. Existing attribution proves page-version WAL reads, per-step native
prepare/close, reset, bind, and refresh are not the dominant cost. The remaining
time is inside native MariaDB/InnoDB execution, but the production probe does
not yet expose enough handler-boundary timing for point-select reads to choose a
safe optimization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` records ownerless prepared-step timing
  around MyLite policy, refresh, bind, dictionary, `mysql_stmt_execute()`, and
  reset. For ownerless prepared `SELECT`, `ownerless_native_prepare_per_step`
  remains false, so native prepare/close is not on the per-step hot path.
- `mariadb/sql/handler.cc` is the SQL-layer handler boundary for key reads:
  `handler::ha_index_read_map()` and `handler::ha_index_read_idx_map()` wrap
  storage-engine index reads and update SQL handler statistics.
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::index_read()`. The relevant point-select stages are statement
  template build, MySQL-key to InnoDB-key conversion, and `row_search_mvcc()`.
- The existing production probe already has SQL handler and InnoDB handler
  perf-counter infrastructure, but point-select read sections only enable the
  higher-level MyLite ownerless database counters.

## Design

Add attribution-only counters for point-select reads:

- SQL handler counters for `ha_index_read_map()` and
  `ha_index_read_idx_map()` call counts and elapsed time.
- InnoDB handler counters for `ha_innobase::index_read()` call count and total
  elapsed time, plus template-build, key-conversion, and `row_search_mvcc()`
  child timings.
- Production probe wiring that enables and resets these counters around
  ordinary and ownerless direct/prepared point-select sections only when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- Compact per-select summaries for ownerless direct/prepared point selects, and
  raw ordinary/ownerless handler stats for side-by-side log comparison.

Do not change SQL behavior, ownerless policy, native read-view lifetime,
page-version visibility, file layout, or public C API behavior.

## Scope And Non-Goals

In scope:

- MariaDB SQL handler and InnoDB handler perf counters;
- production performance-probe attribution output;
- documentation of the next optimization target.

Out of scope:

- changing MariaDB optimizer, handler execution, prepared statement semantics,
  ownerless read-view registration, or InnoDB page-version overlay policy;
- making stats-enabled probe ratios comparable to stats-off throughput ratios;
- broad PHPUnit or WordPress harness changes.

## Compatibility And Storage Impact

No MySQL/MariaDB SQL compatibility change, no `libmylite` API change, no durable
directory-layout change, and no native storage-format change. The counters are
process-local test/probe instrumentation exposed only through first-party test
symbols.

## Test Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced stats-enabled production probe and inspect ordinary versus
  ownerless direct/prepared point-select handler summaries.
- Run a stats-off production probe to confirm normal timing output remains
  available without the attribution flag.
- Run focused ownerless SQL selectors that exercise the read fast path.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Stats-enabled probe output includes SQL handler key-read and InnoDB
  `index_read()` child timings for point-select sections.
- Normal stats-off probe output remains free of handler attribution overhead.
- The slice identifies whether the next point-select optimization belongs at
  the SQL handler boundary, InnoDB handler setup, `row_search_mvcc()`, or a
  surrounding MyLite ownerless hook.

## Implementation Evidence

The MariaDB embedded archive was rebuilt with:

```sh
tools/mariadb-embedded-build build
```

The production probe was rebuilt with:

```sh
cmake --build build/php-embedded-prod --target mylite_embedded_performance_probe
```

A reduced stats-enabled attribution run on 2026-06-18 used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=100 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported one SQL handler `ha_index_read_idx_map()` and one InnoDB
`index_read()` per point select. Ordinary direct/prepared point selects spent
about `0.006 ms/select` in InnoDB `row_search_mvcc()`, while ownerless
direct/prepared point selects spent about `0.012 ms/select`. Ownerless
prepared point selects still reported zero page-version reads, zero native
prepare/close calls, `0.313 ms/select` prepared-step total, and
`0.303 ms/select` in native `mysql_stmt_execute()`.

A stats-off throughput run used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=3000 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported tableless direct/prepared ratios of `0.9221`/`0.8666` and real
InnoDB point-select direct/prepared ratios of `0.8475`/`0.8276`.

## Risks

Stats-enabled attribution adds timer and atomic-counter overhead inside native
hot paths, so those runs must be used for stage attribution, not absolute
throughput ratios. Throughput comparisons must continue to use stats-off
production samples.
