# Range Limit Performance Baseline

## Problem

Short indexed range reads are now row-payload lazy, but the performance harness
does not have a focused phase for `WHERE secondary_key >= ? ORDER BY
secondary_key LIMIT 1`. Without that measurement, the next key-navigation work
cannot distinguish row-materialization gains from the remaining eager key
suffix cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` positions range access
  with `ha_index_read_map()`, and LIMIT handling remains above the handler.
- `tools/mylite_perf_baseline.c` already has direct and prepared primary-key,
  secondary exact, and published-leaf secondary exact phases.
- `tools/mylite_perf_baseline.c::publish_secondary_leaf_index()` creates a
  routed `perf_leaf_rows` table and copy-builds `value_leaf_key`, giving a
  stable published leaf/branch index root for storage-range measurements.
- `tools/mylite_perf_baseline.c::benchmark_secondary_selects_for_index()` and
  `benchmark_prepared_secondary_selects_for_index()` provide the local pattern
  for timing SQL-level secondary index probes and validating row counts and
  checksums.

## Scope

- Add direct and prepared published-leaf secondary range-LIMIT phases to
  `tools/mylite-perf-baseline`.
- Reuse the existing `perf_leaf_rows` published-index setup.
- Time one returned row per iteration using `WHERE value >= ? ORDER BY value,
  id LIMIT 1`.
- Validate the returned row id/value pair deterministically for each iteration.
- Document the new phase in README and storage architecture notes.

## Non-Goals

- No storage or handler behavior change in this slice.
- No default benchmark threshold; thresholds remain opt-in and local.
- No benchmark for non-published append-history range reads.
- No cursor continuation or streaming key-entry implementation.

## Design

The new direct phase will generate a SQL string per iteration, execute it with
`mylite_exec()`, and assert exactly one row whose id/value matches the first
row in the next secondary-key bucket. The prepared phase will bind the lower
bound into the equivalent prepared statement and validate the same result.

Both phases run only after `publish_secondary_leaf_index()` succeeds. If the
published index root is unavailable for a tiny row count, the phase follows the
existing published-leaf exact-select behavior and reports a skip.

## Compatibility Impact

None. This adds measurement coverage for existing SQL behavior; it does not
change SQL, handler, storage, or public API semantics.

## Single-File And Lifecycle Impact

The benchmark continues to use a temporary `.mylite` file and the existing
temporary runtime directory. No persistent sidecars or file-format changes.

## Public API And File-Format Impact

None.

## Storage-Routing Impact

The benchmark uses routed `ENGINE=InnoDB` tables resolved to MyLite, matching
the storage-smoke performance baseline contract.

## Binary-Size, License, And Dependency Impact

Tiny first-party benchmark-tool code only. No dependency or license change.

## Test Plan

- Build `mylite_perf_baseline`.
- Run the new direct and prepared range-LIMIT phases on a small row count.
- Run `tools/mylite-perf-baseline --help` and verify the phase/metric names are
  accepted.
- Run formatting and whitespace checks for the edited benchmark and docs.

## Acceptance Criteria

- The benchmark exposes direct and prepared published-leaf secondary range-LIMIT
  phases.
- Each phase verifies returned rows and prints a stable timing row and checksum.
- Existing benchmark phases and threshold parsing keep working.
- Docs show how to run the new phase.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=direct-leaf-secondary-range-limit-selects 50 5`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-leaf-secondary-range-limit-selects 50 5`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-leaf-secondary-range-limit-selects --max-us=prepared-leaf-secondary-range-limit-selects:100000 50 5`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --help 2>&1 | rg "direct-leaf-secondary-range-limit-selects|prepared-leaf-secondary-range-limit-selects"`
- `git diff --check`
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`

## Risks And Open Questions

- The phase measures SQL-level range access and therefore includes MariaDB
  planning/execution overhead, not only storage cursor work.
- A later key-continuation slice should add lower-level storage counters or
  component timings if SQL-level numbers are too coarse.
