# Range Tail Performance Baseline

## Problem

Bounded range `LIMIT 1` reads now use published leaf and branch roots even when
an append-tail overlay exists. The local performance harness measures the
static published-root range shape, but it does not isolate the same SQL shape
after new rows have been appended beyond the published root. That leaves the
remaining eager-tail cost hard to measure before a later tail index or durable
merge cursor design.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` and
  `handler::read_range_next()` drive the same handler range path for static
  roots and roots with append-tail overlays.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()` uses
  the bounded storage range API for byte-safe raw forward lower-bound cursors.
- `tools/mylite_perf_baseline.c` already has direct and prepared published-leaf
  secondary range `LIMIT 1` phases over `perf_leaf_rows`.

## Scope

- Add direct and prepared benchmark phases for published-leaf secondary range
  `LIMIT 1` reads after appending a deterministic tail beyond the published
  root.
- Keep the benchmark result rows identical to the static-root range phases by
  appending tail rows whose secondary keys sort after the measured lower-bound
  buckets.
- Document the new phases in README, storage architecture notes, and the
  roadmap.

## Non-Goals

- No storage format change.
- No tail index, merge cursor, or range cursor implementation change.
- No default threshold for the new metrics; thresholds remain opt-in and
  machine-local.

## Design

The harness will publish the existing secondary leaf root, then append
`max(1, rows / 10)` additional rows to the same table. Tail rows use primary
keys above the base row set and secondary values above the normal benchmark
bucket range. Existing range `LIMIT 1` queries therefore still return the base
row `(id=value, value=value)` for each measured bucket while forcing storage to
include append-tail overlay pages in the cursor path.

The new phase names and metric names are:

- `direct-leaf-secondary-tail-range-limit-selects`
- `prepared-leaf-secondary-tail-range-limit-selects`

The setup metric `prepare-leaf-tail-rows` records tail-row append cost outside
the measured select loop.

## Compatibility Impact

No SQL-visible behavior changes. The benchmark asserts the same result checks
as the existing range `LIMIT 1` phases.

## Single-File And Lifecycle Impact

The benchmark creates ordinary MyLite rows and index-entry tail pages in the
primary `.mylite` file. It introduces no companion files.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Routing Impact

The benchmark continues to route omitted `ENGINE=InnoDB` tables through the
static MyLite storage engine under the storage-smoke embedded build.

## Binary-Size, License, And Dependency Impact

First-party benchmark and documentation only. No dependency or license change.

## Test Plan

- Build `mylite_perf_baseline`.
- Run the two new phases on a small row count.
- Run `--help` and verify the phase and metric names are listed.
- Run formatting and whitespace checks for the touched files.

## Acceptance Criteria

- The new direct and prepared tail-overlay phases execute successfully and
  verify result checksums.
- The harness prints the tail setup metric and stable tail phase metric names.
- README, storage architecture notes, and roadmap mention the tail-overlay
  phases.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=direct-leaf-secondary-tail-range-limit-selects 50 5`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-leaf-secondary-tail-range-limit-selects 50 5`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=all 10 2`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --help 2>&1 | rg "direct-leaf-secondary-tail-range-limit-selects|prepared-leaf-secondary-tail-range-limit-selects|prepare-leaf-tail-rows"`

## Risks And Open Questions

- This measures the eager tail cost but does not reduce it. The later storage
  design still needs a durable tail index or merge cursor to avoid scanning long
  append tails.
