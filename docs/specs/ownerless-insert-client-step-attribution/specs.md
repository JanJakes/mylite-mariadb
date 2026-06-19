# Ownerless Insert Client Step Attribution

## Problem

Ownerless write-throughput work has compact native page-publish, page-log,
commit, and deep InnoDB counters, but the production embedded performance probe
still treated prepared insert execution as one client-side timing window. That
left an attribution gap: the branch could distinguish page-version publication
and native InnoDB subphases, but not how much of the remaining elapsed insert
time sat in the `libmylite` prepared-statement call sequence itself.

This slice adds stats-enabled client/probe timing around prepared insert
`prepare`, `START TRANSACTION`, `bind`, `step`, `reset`, `COMMIT`, and
`finalize` calls for the existing ordinary and ownerless transactional and
autocommit insert probe paths.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc:8083` reaches
  `row_insert_for_mysql()` for handler inserts, and
  `mariadb/storage/innobase/row/row0mysql.cc:1228` implements that InnoDB row
  insert path before it drives `row_ins_step()`.
- `mariadb/storage/innobase/trx/trx0trx.cc:1485` implements
  `trx_t::write_serialisation_history()`, and
  `mariadb/storage/innobase/trx/trx0trx.cc:2476` implements
  `trx_commit_for_mysql()`. Existing ownerless deep counters already attribute
  those native insert and commit paths.
- `packages/libmylite/include/mylite/mylite.h` exposes the prepared statement
  calls used by the probe: `mylite_prepare()`, `mylite_step()`,
  `mylite_reset()`, `mylite_clear_bindings()`, and `mylite_bind_int64()`.
- `packages/libmylite/tests/embedded_performance_probe.c` uses those calls in
  `measure_transactional_insert()` and `measure_autocommit_insert()`, with the
  current timing windows starting after `START TRANSACTION` for explicit
  transactions and immediately before the bind/step loop for autocommit.

## Design

Add a local `insert_client_timing` accumulator to the embedded performance
probe. The accumulator is optional and is passed only when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, matching the existing reduced
ownerless attribution probe. Stats-off probe runs keep the previous timing
path and do not pay per-step `monotonic_ns()` overhead.

The instrumentation keeps the existing measured throughput windows:

- explicit transaction measured time remains the loop plus `COMMIT`, excluding
  `prepare`, `START TRANSACTION`, and `finalize`;
- autocommit measured time remains the bind/step/reset loop, excluding
  `prepare` and `finalize`.

The probe emits compact `mylite_perf_summary_*_client_*` rows for ordinary and
ownerless transactional and autocommit prepared inserts. It also emits
ownerless-minus-ordinary client timing deltas for the transactional and
autocommit paths. `measured_residual_ms_per_insert` reports measured time not
accounted for by bind, step, reset, and commit timing; it intentionally excludes
prepare, begin, and finalize because those calls are outside the existing
measured loop windows.

## Compatibility Impact

No SQL behavior, `libmylite` public API behavior, storage-engine behavior,
metadata routing, or directory lifecycle changes. The slice changes only
performance-probe diagnostics.

## Database Directory And Embedded Lifecycle Impact

No durable directory-layout changes. The performance probe continues to create
and remove its temporary MyLite directory and database path.

## Native Storage Impact

No native storage format or recovery behavior changes. Existing InnoDB insert,
commit, history-proof, page-publish, and page-log code paths are unchanged.

## Wire Protocol Or Integration-Package Impact

No wire-protocol, PHP, mysqli, or WordPress harness behavior changes.

## Build And Performance Impact

Production probe binaries grow by a small local accumulator and summary print
helpers. Stats-off runs do not execute the per-step timers. Stats-enabled
attribution runs execute additional `monotonic_ns()` calls inside the existing
measured insert windows, so their throughput remains attribution evidence
rather than the branch's stats-off throughput signal.

## Test And Verification Plan

- Build the production embedded performance probe.
- Run a reduced stats-enabled production embedded performance probe and verify
  the new ordinary, ownerless, and ownerless-minus-ordinary client summary
  keys appear.
- Run a reduced stats-off production embedded performance probe to verify the
  normal throughput path still executes without the optional accumulator.
- Run production-build guard and formatting/diff checks.

## Verification Results

Local production verification on 2026-06-19:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20
  MYLITE_PERF_INSERT_ITERATIONS=20
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  passed and wrote `build/manual-insert-client-step-attribution-stats.txt`.
  The output included ordinary transactional, ordinary autocommit, ownerless
  transactional, ownerless autocommit, and ownerless-minus-ordinary client
  timing summaries.
- In that reduced stats-enabled sample, ownerless transactional prepared
  inserts reported `0.494 ms/insert` measured client loop time versus
  `0.283 ms/insert` ordinary, with the ownerless-minus-ordinary delta mostly
  in `step` (`0.181 ms/insert`) and `COMMIT` (`0.030 ms/insert`). Ownerless
  autocommit prepared inserts reported `1.299 ms/insert` measured client loop
  time versus `0.341 ms/insert` ordinary, with the ownerless-minus-ordinary
  delta mostly in `step` (`0.957 ms/insert`).
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20
  MYLITE_PERF_INSERT_ITERATIONS=20
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  passed and wrote
  `build/manual-insert-client-step-attribution-stats-off.txt`. That stats-off
  output retained the existing write-throughput summaries and did not emit
  `mylite_perf_summary_*_client_*` rows.
- `tools/check-ci-production-builds`, `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure`, `cmake --build
  --preset format-check-prod`, `git diff --check`, and `git diff --cached
  --check` passed.

## Acceptance Criteria

- Stats-enabled probe output includes ordinary transactional, ordinary
  autocommit, ownerless transactional, ownerless autocommit, and
  ownerless-minus-ordinary client step timing rows.
- Existing throughput summary keys remain present.
- Stats-off probe runs do not require or emit the client-step attribution rows.
- Documentation records the new keys as diagnostic instrumentation and does not
  claim a performance improvement.

## Risks And Unresolved Questions

- Per-step timers add overhead to stats-enabled samples, especially short
  reduced runs. Use stats-off samples for throughput comparisons and
  stats-enabled samples for attribution.
- The remaining ownerless write-throughput target is not solved by this slice;
  the new rows make the next optimization target easier to choose.
