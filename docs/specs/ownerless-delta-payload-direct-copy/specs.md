# Ownerless Delta Payload Direct Copy

## Problem Statement

The current production ownerless attribution probe shows page-log append CPU is
still visible after the larger WAL-format reductions. A reduced 500-row
stats-enabled sample at branch head `c0431498` reported `1506` page-log append
calls, `17.627ms` in append encoding, and `4.954ms` in delta encoding, with
WAL payload already stable around `1112` bytes per insert.

The remaining delta encoder builds the final payload header first, then appends
each changed-byte run with `std::vector::insert()`. That keeps the durable
record correct but pays repeated vector append machinery even though the total
changed-byte count is known before raw bytes are copied.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` still supplies the stable committed
  page image passed synchronously to MyLite's page-log append hook.
- `packages/libmylite/src/ownerless_page_log.cc::build_index_delta_payload()`
  scans index and undo pages, records changed-byte runs, writes the durable
  base-record offset and varuint run descriptors, and then appends raw changed
  bytes in run order.
- The same helper is used for both `FIL_PAGE_INDEX` and `FIL_PAGE_UNDO_LOG`
  non-chained page deltas. `FIL_PAGE_TYPE_SYS` pages stay standalone after the
  SYS-delta rejection slice.
- Existing primitive coverage already verifies fast index deltas, exact
  fallback deltas, undo-log deltas, checkpoint rewrite to standalone records,
  and byte-exact readback through direct record reads and latest-page lookup.

## Design

Keep the page-log record format, delta payload layout, base-record offset,
checkpoint rewrite behavior, and replay behavior unchanged.

When `build_index_delta_payload()` finishes collecting changed-byte runs, it
already has the total raw changed-byte count. Resize the output vector once for
that raw tail and copy each run directly into the reserved tail with
`std::memcpy()` instead of repeatedly calling `std::vector::insert()` at the
end.

This is a CPU-only encoder change. The byte sequence written after the run
descriptors remains the same concatenation of raw changed-byte runs.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli API, wire-protocol behavior, storage
format, directory layout, or native InnoDB behavior changes. Existing page-log
records continue to decode through the same delta and checkpoint paths.

## Directory And Lifecycle Impact

No new files, shared-memory fields, durable metadata, startup behavior, close
behavior, or process ownership rules change.

## Native Storage Impact

Native page images, redo, undo, checkpoints, and crash recovery are unchanged.
The slice only changes how MyLite materializes the in-memory payload buffer
before writing the same page-version WAL record.

## Build And Performance Impact

The change removes repeated `vector::insert()` calls from index/undo delta
encoding. The expected effect is a small reduction in
`mylite_perf_*_page_log_append_delta_encode_ms` without changing append counts,
delta counts, payload bytes, or checksum behavior.

## Test And Verification Plan

- Build the production ownerless primitive test and embedded performance probe.
- Run `libmylite.ownerless-primitives` under `php-embedded-prod` to cover
  byte-exact index, undo, fast, exact, and checkpoint-rewritten deltas.
- Run a reduced stats-enabled production embedded performance probe and compare
  page-log append counts, payload bytes, delta counts, and delta encode timing.
- Run focused production ownerless SQL selectors for the history WAL proof,
  native-support page WAL elision, visible-fast insert, and uncommitted peer
  visibility.
- Run the production-build audit, format check, and whitespace check.

## Verification Results

Local verification on 2026-06-16 used `php-embedded-prod` first-party Release
artifacts and the existing `MinSizeRel` MariaDB embedded archive:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` kept
  `1506` page-log appends, `556204` payload bytes, `481` index delta records,
  and `376` undo delta records for the ownerless autocommit insert phase. The
  same sample reported delta encode time at `4.556ms`, compared with the
  pre-slice same-shape local sample's `4.954ms`.
- Focused production selectors passed:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test`
  passed.
- Focused hook selectors passed:
  `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$'
  --output-on-failure`.
- The first full `ctest --preset ownerless-stress --output-on-failure` run was
  stopped after `temp-stress` sat in a futex wait for over seven minutes. After
  cleanup, a direct `timeout 360s
  build/ownerless-stress/packages/libmylite/mylite_ownerless_cross_process_sql_test
  temp-stress` rerun passed in under eight seconds.
- A full stress rerun with an outer timeout passed:
  `timeout 900s ctest --preset ownerless-stress --output-on-failure`, 12/12
  tests in `255.23s`.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Existing page-log primitive tests pass unchanged.
- Reduced production attribution still reports stable page-log append counts
  and payload bytes for ownerless autocommit inserts.
- Delta records remain selected for the same index and undo hot-path shapes.
- No checkpoint, replay, recovery, or SQL behavior changes are introduced.

## Risks And Follow-Up

This is a bounded CPU cleanup, not a fix for the larger remaining performance
targets. Native commit/page-publication cost, remaining page-version volume,
and broader redo/checkpoint reconciliation remain the higher-impact ownerless
performance work.
