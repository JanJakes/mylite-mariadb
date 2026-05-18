# Prepared Performance Baseline

## Problem Statement

The first routed-storage performance harness reports only direct SQL timings.
That keeps the initial baseline simple, but it leaves prepared-statement
application paths unmeasured. Prepared routed `SELECT` correctness now has
focused coverage, so the benchmark can add prepared timing rows without mixing
them into direct-operation measurements.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/include/mylite/mylite.h` exposes the public prepared
  statement API used by applications: `mylite_prepare()`, scalar binding,
  `mylite_step()`, typed column access, `mylite_reset()`, and
  `mylite_finalize()`.
- `docs/specs/prepared-routed-select-reads/specs.md` records current coverage
  that prepared routed reads see the same MyLite-routed table rows as direct
  reads.
- `tools/mylite_perf_baseline.c` already creates an `ENGINE=InnoDB` table under
  the storage-smoke profile, which is the application-shaped routed storage path
  to measure.

## Scope

- Add prepared primary-key point-select timing over the existing benchmark
  table.
- Add prepared primary-key update timing inside one transaction.
- Reuse one prepared statement per measured operation and reset/rebind each
  iteration through the public `libmylite` API.
- Keep direct and prepared timings as separately labelled rows.

## Non-Goals

- Add CI thresholds or product performance claims.
- Compare against SQLite, MariaDB, or MySQL.
- Optimize storage, handler cursor reuse, B-tree navigation, WAL, or page
  cache behavior.
- Add prepared insert timing; the direct insert row remains the initial load
  baseline until a separate insert-focused benchmark slice exists.

## Compatibility Impact

No SQL compatibility behavior changes. This is a developer measurement tool
change over already-covered public prepared statement behavior.

## Single-File And Embedded Lifecycle

The benchmark still creates one temporary `.mylite` file and removes the
temporary tree before exit. It does not add new durable companions or runtime
lifecycle rules.

## Test And Verification Plan

- `tools/mylite-perf-baseline`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror tools/mylite_perf_baseline.c`
- `git diff --check`

## Acceptance Criteria

- Baseline output includes separate prepared primary-key point-select and
  prepared primary-key update rows.
- Prepared point selects produce a checksum so successful execution cannot be a
  no-op.
- Prepared updates run inside one transaction and preserve the expected row
  count.
- The benchmark still cleans up its temporary file tree.

## Risks And Open Questions

- Results still combine MariaDB embedded SQL-layer overhead with MyLite storage
  overhead. Lower-level storage microbenchmarks remain future work.
- Prepared statement reset/rebind cost is included because that is the current
  public API contract; future API or implementation changes can add narrower
  measurements.
