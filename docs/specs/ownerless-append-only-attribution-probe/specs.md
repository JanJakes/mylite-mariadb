# Ownerless Append-Only Attribution Probe

## Problem

The production embedded ownerless attribution probe enables
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. That keeps stable page-version
volume and commit-visibility counters, but the MariaDB mini-transaction path
intentionally disables `mtr_t::ownerless_history_proof_publish_pair()` while
page-publish stats are active. As a result, CI can prove the conservative
instrumented publication path, but it cannot directly report the production
history-proof pair append counters and timing from the stats-off fast path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_history_proof_publish_pair()` returns without publishing a
  pair when `ownerless_page_publish_stats_enabled` is set. This keeps detailed
  page-publish attribution deterministic, but it is not the production fast
  path.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_history_proof_publish_pair_hook()` records
  `history_proof_pair_*` database counters and appends the two proof-only
  native-support records through the page-log append-session pair API when an
  active session is available.
- `packages/libmylite/tests/embedded_performance_probe.c` already exposes the
  database and page-log append counters, but only enables them for ownerless
  insert phases through the page-publish stats mode.

## Design

Add `MYLITE_PERF_OWNERLESS_APPEND_STATS=1` to the embedded performance probe.
This mode enables only the ownerless database, page-log append, page-log scan,
page-log sync, and bulk `mylite_exec()` counters for ownerless insert phases.
It does not enable page-publish, page-write, commit-visibility, SQL-handler,
InnoDB-handler, or deep InnoDB instrumentation. If page-publish stats are also
requested, append-only mode stays off because page-publish stats change the
measured ownerless history-proof path.

CI keeps the existing stats-enabled ownerless attribution probe and adds a
separate production-build append attribution step that writes
`build/embedded-performance-reports/ownerless-append-attribution.log`.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, DDL, directory-layout, or storage behavior
changes. This is diagnostic instrumentation for performance comparison only.

## Directory And Lifecycle Impact

No new durable files or runtime directories are introduced. The CI artifact is
a build output log under `build/embedded-performance-reports/`.

## Native Storage Impact

Native MariaDB/InnoDB files, page formats, redo ordering, and ownerless WAL
records are unchanged. The append-only probe observes the production
history-proof pair hook instead of changing it.

## Performance Impact

The new mode has measurement overhead when requested, but it keeps the
page-publish stats gate off so the measured ownerless insert path still uses
production history-proof pair publication. Default stats-off throughput probes
remain unchanged.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` with the `embedded-prod` preset.
- Run a reduced production append-only probe with
  `MYLITE_PERF_OWNERLESS_APPEND_STATS=1` and verify that
  `mylite_perf_ownerless_page_publish_stats=0`,
  `mylite_perf_ownerless_append_stats=1`, and ownerless autocommit
  `history_proof_pair_calls` are nonzero.
- Run `tools/check-ci-production-builds` so the new CI timing step keeps the
  production-build guards and artifact path.
- Run formatting and diff whitespace checks.

## Acceptance Criteria

- Append-only ownerless attribution can be collected without enabling
  page-publish stats.
- CI publishes the new append attribution log alongside the existing embedded
  performance reports.
- The existing page-publish attribution probe remains available for stable
  page-version and commit-visibility counters.
- Documentation continues to describe this as measurement evidence, not as
  completion of the broader ownerless concurrency objective.

## Verification

- `cmake --build --preset embedded-prod --target
  mylite_embedded_performance_probe` passed.
- Reduced production append-only probe passed:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20
  MYLITE_PERF_INSERT_ITERATIONS=100 MYLITE_PERF_OWNERLESS_APPEND_STATS=1
  build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe`.
  The filtered output included `mylite_perf_ownerless_page_publish_stats=0`,
  `mylite_perf_ownerless_append_stats=1`,
  `mylite_perf_ownerless_insert_autocommit_history_proof_pair_calls=100`,
  `mylite_perf_ownerless_insert_autocommit_history_proof_pair_succeeded=100`,
  and `mylite_perf_summary_ownerless_autocommit_history_proof_pair_calls_per_insert=1.000`.
- `tools/check-ci-production-builds` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` and
  `tools/require-cmake-release-build build/embedded-prod` passed.
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.
