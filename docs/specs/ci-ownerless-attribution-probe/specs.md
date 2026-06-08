# CI Ownerless Attribution Probe

## Problem Statement

The embedded performance probe now emits per-insert ownerless autocommit phase
summaries when `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, but the CI
embedded job still runs only the default stats-off performance probe. That is
the right throughput sample, because detailed stats perturb timings, but it
means CI logs do not show the new ownerless attribution keys.

This slice adds a separate CI step that runs the same production-built
embedded probe with reduced iteration counts and detailed ownerless stats
enabled. The existing stats-off step remains the throughput signal.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` configures and builds the embedded job with
  `php-embedded-prod`, so the existing embedded performance probe already uses
  production MyLite binaries and the optimized MariaDB embedded archive.
- `packages/libmylite/tests/embedded_performance_probe.c` defaults detailed
  ownerless stats off and emits top-level `mylite_perf_summary_*` throughput
  keys in that mode.
- The same probe emits detailed ownerless page-publish, page-log, page-write,
  database, SQL handler, InnoDB handler, and deep InnoDB counters when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- The `ownerless-autocommit-phase-summary` slice adds derived
  `mylite_perf_summary_ownerless_autocommit_*` keys in stats-enabled mode.

## Design

Keep the existing CI step:

```yaml
- name: Run embedded performance probe
  run: build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

Add a follow-up step:

```yaml
- name: Run embedded ownerless attribution probe
  env:
    MYLITE_PERF_OPEN_CLOSE_ITERATIONS: "1"
    MYLITE_PERF_SELECT_ITERATIONS: "100"
    MYLITE_PERF_INSERT_ITERATIONS: "100"
    MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS: "1"
  run: build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The reduced attribution probe is not a throughput gate. Its purpose is to make
the production CI log expose page-version volume, native-support ratio,
page-publish/page-log append time, commit-MTR publish time, InnoDB
write-history time, ownerless visibility time, and row-insert time without
manual local reruns.

## Scope

In scope:

- CI workflow split between stats-off embedded throughput and stats-enabled
  ownerless attribution.
- Documentation updates for performance interpretation.
- Local command verification of the reduced stats-enabled probe.

Out of scope:

- Engine behavior changes.
- New performance thresholds.
- WordPress PHPUnit harness changes.
- Running a full stats-enabled ownerless SQL suite.

## Compatibility Impact

No SQL, C API, PHP API, storage-engine, directory-lifecycle, locking, recovery,
or checkpoint behavior changes. CI emits additional timing logs only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The performance probe still creates and
removes its temporary database directory.

## Native Storage Impact

No native storage format changes.

## Public API Impact

No public API changes.

## Build And Binary-Size Impact

No binary-size impact. CI runs an already-built production test/probe binary
one extra time with reduced iteration counts.

## Test Plan

- Run the reduced stats-enabled production probe with the CI attribution
  environment and confirm `mylite_perf_summary_ownerless_autocommit_*` keys
  are emitted.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The existing CI embedded performance step remains stats-off.
- CI has a separate production-built ownerless attribution probe step.
- The attribution step uses explicit reduced iteration counts and enables
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- Docs distinguish throughput timing from stats-enabled attribution evidence.

## Verification Results

Local verification on 2026-06-08 used the existing production
`build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
binary.

- CI-shaped attribution command passed:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`.
- The local sample emitted the expected attribution keys, including
  `mylite_perf_summary_ownerless_autocommit_page_versions_per_insert=7.810`,
  `mylite_perf_summary_ownerless_autocommit_native_support_page_ratio=0.8720`,
  `mylite_perf_summary_ownerless_autocommit_write_history_ms_per_insert=1.976`,
  and `mylite_perf_summary_ownerless_autocommit_row_insert_ms_per_insert=0.574`.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Risks And Unresolved Questions

- Stats-enabled timing is attribution evidence, not throughput evidence.
- CI runner load can still affect per-insert timing values; compare trends
  against the stats-off throughput step and repeated samples.
