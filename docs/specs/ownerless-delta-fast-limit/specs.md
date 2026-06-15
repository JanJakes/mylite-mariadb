# Ownerless Delta Fast Limit

## Problem

After the page-delta fast-miss reuse and standalone-size probe slices, the
ownerless page-log append path still spent measurable time in exact fallback.
A 500-row production stats-enabled sample at branch head `47145325` reported
`1506` ownerless autocommit page-log appends, `91` standalone-size probe calls,
`84` exact deltas reusing a retained fast-miss payload, and `7.229 ms` in the
size-only standalone probe. Payload bytes were already stable at about
`1112` bytes per insert, so the next bounded target was the fast/exact decision
heuristic rather than a WAL-format change.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` still provides the stable committed
  page images passed to the synchronous MyLite page-log append hook.
- `packages/libmylite/src/ownerless_page_log.cc` keeps non-chained index and
  undo page deltas keyed to durable standalone base records. Fast acceptance
  skips current standalone materialization only when the delta is less than
  half the recorded standalone base payload and below the configured fast
  payload limit.
- Exact fallback already remains available for larger or uncertain deltas and
  compares a retained fast-miss delta with the current standalone payload size
  before selecting it.

## Design

Raise the fast delta payload limit from `1024` bytes to `2048` bytes. The
existing half-standalone-size rule, durable standalone-base requirement,
non-chained delta format, checkpoint rewrite behavior, and exact fallback all
remain unchanged.

The wider limit is a runtime heuristic. It may turn some deltas that would have
paid exact fallback and size probing into fast-accepted deltas, but it does not
make process-local base-cache state durable and does not change replay rules.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, wire-protocol, storage-format, directory-layout,
or native InnoDB behavior changes. The page-log record format is unchanged.
The optimization only changes when an already-valid delta payload can be
selected before materializing or probing the current standalone payload.

## Native Storage And Lifecycle Impact

No native InnoDB page, redo, undo, checkpoint, or purge format changes. No new
files or shared-memory fields are added. A process restart or dropped delta-base
cache simply returns the append path to standalone warm-up and exact fallback.

## Test Plan

- Build the production ownerless primitive test and embedded performance probe.
- Run `libmylite.ownerless-primitives` to keep byte-exact fast delta and
  exact-fallback reconstruction covered.
- Run a reduced production stats-enabled embedded performance probe and compare
  fast/exact delta, size-probe, payload-byte, and append-time counters against
  the pre-slice sample.
- Run a reduced production stats-off embedded performance probe as a throughput
  sanity check.
- Run adjacent ownerless SQL coverage, production build guards, format check,
  and whitespace check.

## Verification Results

Local verification on 2026-06-15 used `php-embedded-prod` first-party Release
artifacts and the existing `MinSizeRel` MariaDB embedded archive:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- Focused production selectors passed:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure`.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` preserved
  `1506` page-log append calls and essentially unchanged payload bytes
  (`556202` versus `556208` pre-slice). Fast index deltas increased from `410`
  to `449`, exact index deltas dropped from `71` to `32`, fast-limit rejections
  dropped from `43` to `1`, standalone-size probe calls dropped from `91` to
  `51`, size-probe time dropped from `7.229 ms` to `1.157 ms`, page-log append
  encode time dropped from `22.498 ms` to `19.103 ms`, and total page-log
  append time dropped from `49.012 ms` to `44.932 ms`.
- A reduced stats-off production sanity probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ownerless autocommit at
  `1582.17 ops/s`, ordinary autocommit at `3542.99 ops/s`, ownerless
  transactional inserts at `2607.67 ops/s`, and ownerless bulk row-list inserts
  at `4018.95 rows/s`. Treat this as throughput sanity evidence; CI still uses
  the production probe over comparable runners for trend comparison.
- Hook-build focused selectors passed after the visible-fast path test was
  tightened to require exact direct-plus-session append accounting while
  tolerating hook-only direct append paths:
  `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$'
  --output-on-failure`.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed with the local
  clang-format runtime library path.
- `git diff --check` passed.

## Acceptance Criteria

- The fast limit moves to `2048` bytes without changing WAL format or replay.
- Primitive coverage still proves byte-exact fast delta readback and exact
  fallback above the new threshold.
- Production attribution shows fewer exact fallbacks and no payload-byte
  regression in the representative ownerless autocommit sample.

## Risks And Follow-Up

The fast limit remains a heuristic. Broader workloads could prefer a lower or
adaptive limit if payload size and CPU move differently. The larger remaining
performance work is still native commit/page-publication cost, remaining
non-fast encoding, and redo/checkpoint reconciliation.
