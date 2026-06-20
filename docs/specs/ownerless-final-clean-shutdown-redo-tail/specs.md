# Ownerless Final Clean-Shutdown Redo Tail

## Problem

Clean-shutdown redo-tail truncation removed repeated ordinary warm-open redo
rebuilds caused by an 8-byte physical `ib_logfile0` tail, but reduced
production probes still showed occasional ownerless warm-open rebuilds with the
same signature: size mismatch only, physical `ib_logfile0` at `100663304`
bytes, configured redo size at `100663296` bytes, and matching redo format.

The ownerless path differs from ordinary embedded opens because read/write
ownerless startup configures `innodb_fast_shutdown=2`. MariaDB treats that mode
as crash-like shutdown and returns before the clean checkpoint and tail
normalization code can run.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/srv0srv.h` documents
  `innodb_fast_shutdown=2` as effectively crashing the server with no log
  checkpoint.
- `mariadb/storage/innobase/log/log0log.cc:logs_empty_and_mark_files_at_shutdown()`
  logs the `innodb_fast_shutdown=2` path, flushes the redo buffer, sets
  `SRV_SHUTDOWN_LAST_PHASE`, records the fast-shutdown bucket, and returns
  before `log_make_checkpoint()` and the embedded clean-shutdown redo-tail
  truncation helper.
- `packages/libmylite/src/database.cc:runtime_arguments()` starts ownerless
  read/write runtimes with `--innodb-fast-shutdown=2`, while ordinary
  read/write opens use `--innodb-fast-shutdown=1`.
- `packages/libmylite/src/database.cc:release_runtime()` already takes the
  ownerless startup lock and computes `no_live_ownerless_shutdown` before
  embedded server shutdown. That is the point where MyLite can distinguish a
  final ownerless close from a live-peer close.

## Design

Keep ownerless read/write runtime startup configured with
`--innodb-fast-shutdown=2` so live-peer ownerless closes retain the existing
crash-like shutdown policy while another ownerless process may still be active.

For a final ownerless close only:

- require the ownerless runtime startup lock;
- require `ownerless_runtime_has_no_live_peers()`;
- require no retained ownerless page-version WAL payload records after
  close-time reclaim;
- temporarily change MariaDB's `srv_fast_shutdown` from `2` to `1` before
  `mysql_server_end()`;
- restore the previous value after `mysql_server_end()` returns;
- allow the existing saved redo-header repair path when retained ownerless WAL
  is metadata-only, while continuing to block that repair when retained
  page-image WAL payload records remain.

This lets the final ownerless process run MariaDB's normal clean-shutdown
checkpoint path and the embedded redo-tail truncation helper, without changing
startup rebuild predicates or live-peer shutdown policy.
Metadata-only ownerless WAL records, such as native-support proof or baseline
records with no page image payload, do not block this clean shutdown path
because they are not page-version recovery payloads. Retained page-image WAL
payload records still keep the crash-like shutdown policy until a later
native-checkpoint/reclaim slice proves the payload can be dropped.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, or WordPress behavior changes.
The intended effect is lower ownerless process startup variance by avoiding a
size-mismatch redo rebuild after a clean final ownerless close.

## Directory And Lifecycle Impact

No directory layout change. The change applies only while closing the final
ownerless read/write runtime for a MyLite database directory and only when
retained ownerless page-version WAL payload has already been reclaimed.

## Native Storage Impact

The final ownerless close now runs the same MariaDB clean InnoDB shutdown class
as ordinary embedded opens for the native redo file when no page-image WAL
payload remains. The existing saved redo-header repair path follows the same
payload-aware gate. Live-peer ownerless closes continue using the existing
`innodb_fast_shutdown=2` behavior.

## Build And Size Impact

The slice adds one embedded-only reference to MariaDB's `srv_fast_shutdown`
global and focused lifecycle tests. It adds no dependency and no public API.

## Test And Verification Plan

- Build `mylite_embedded_open_close_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the ownerless-directory lifecycle selector and confirm repeated
  ownerless final closes leave `datadir/ib_logfile0` at `100663296` bytes.
- Run a reduced production performance probe and confirm ownerless warm
  open/close reports zero actual redo rebuilds in the focused sample.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Final ownerless close runs clean shutdown only when no live ownerless peers
  are present and no retained ownerless page-version WAL payload remains.
- Live-peer ownerless close keeps the configured `innodb_fast_shutdown=2`
  policy.
- Focused ownerless lifecycle coverage asserts native redo file size remains at
  the configured size across repeated final ownerless closes for both
  ownerless-created write payload and ordinary-created metadata-only ownerless
  attach paths.
- The reduced production probe no longer reports ownerless warm-open redo
  rebuilds for the physical 8-byte tail signature.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_open_close_test mylite_embedded_performance_probe
  -j$(nproc)` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_embedded_open_close_test
  ownerless-directory` passed.
- `ctest --preset php-embedded-prod -R
  '^(libmylite\.embedded-open-close|libmylite\.embedded-ownerless-directory-lifecycle|libmylite\.embedded-ownerless-product-hooks|libmylite\.ownerless-primitives|tools\.ownerless-transaction-stress-trace)$'
  --output-on-failure` passed.
- Reduced production performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`, `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=1`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=0` reported:
  - `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_log_rebuild_calls=0`;
  - `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_log_rebuild_total_ms_avg=0.000`;
  - `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_log_rebuild_size_mismatch_calls=0`;
  - `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_srv_start_total_ms_avg=41.034`;
  - `mylite_perf_summary_ownerless_warm_open_close_ms_avg=111.688`;
  - `mylite_perf_summary_ordinary_warm_open_close_ms_avg=113.698`.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

This slice deliberately does not make live-peer ownerless closes run a native
clean shutdown. If future evidence shows live-peer clean checkpoints are safe
and useful, that must be a separate redo/checkpoint reconciliation slice with
multi-process correctness coverage.
