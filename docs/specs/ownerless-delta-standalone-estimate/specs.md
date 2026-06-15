# Ownerless Delta Standalone Estimate Refresh

## Problem

After the ownerless delta fast-limit slice, the representative 500-row
production stats-enabled probe still reported `51` standalone-size probes and
`44` exact fallbacks that reused a retained fast-miss delta payload. Those
fallbacks already paid to compute the current page's standalone payload size,
but the process-local delta-base cache kept using only the original standalone
base record's payload size for later fast decisions.

When the original base image is cheaply encoded but later page versions encode
larger as standalone records, that stale estimate can keep later valid deltas
on the exact fallback path even after exact fallback has proved a larger current
standalone size for the same durable base.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` remains the native source of stable
  committed page images passed to MyLite's synchronous page-log append hook.
- `packages/libmylite/src/ownerless_page_log.cc` keeps index and undo deltas
  non-chained: every delta references a durable standalone base record and
  checkpoint rewrite materializes retained deltas as standalone records.
- `append_record_at_locked()` first tries the fast delta decision against the
  process-local base cache, then exact fallback can run
  `encoded_payload_size_for_page_probe()` before selecting a retained fast-miss
  delta.
- Before this slice, `note_index_delta_base_after_successful_append()`
  incremented the delta count after delta appends but did not refresh the base
  cache's standalone-size estimate from an exact fallback probe.

## Design

Keep the WAL record format, delta payload format, durable base-record proof,
checkpoint rewrite, and replay behavior unchanged.

When exact fallback computes a current standalone payload size for a retained
fast-miss delta, carry that size to the post-append delta-base note step. If
the append succeeds and the record is a delta for the matching process-local
base slot, update the slot's standalone-size estimate to the probed current
size before later appends use the fast decision.

The estimate is process-local and advisory. It is reset by standalone base
refresh, checkpoint/log-generation invalidation, process restart, or cache
eviction. Exact fallback remains available whenever the fast decision rejects a
candidate. Because the record remains a normal non-chained delta against the
same durable standalone base, recovery and checkpoint semantics do not depend
on the estimate.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, wire-protocol, storage-format,
directory-layout, or native InnoDB behavior changes. The change can only alter
whether an already-valid delta payload is selected through the fast path or the
exact fallback path inside one process.

## Native Storage And Lifecycle Impact

No native InnoDB page, redo, undo, checkpoint, purge, or tablespace format
changes. No durable files or shared-memory fields are added. A restart simply
returns to standalone warm-up and exact fallback until the process-local cache
learns new estimates again.

## Test Plan

- Add primitive coverage where the first append must exact-probe and accept a
  retained delta because the original base's standalone encoding is too small,
  then a later same-base append fast-accepts because the estimate was refreshed.
- Run `libmylite.ownerless-primitives`.
- Run a reduced production stats-enabled embedded performance probe and compare
  fast/exact delta, standalone-size probe, payload-byte, and append-time
  counters against the pre-slice sample.
- Run adjacent ownerless SQL coverage, hook coverage, production build guards,
  format check, and whitespace checks.

## Acceptance Criteria

- Exact fallback still compares retained fast-miss deltas against the current
  standalone payload size before selecting them.
- Successful delta appends can refresh the process-local standalone-size
  estimate only after exact fallback has computed the current standalone size.
- Primitive coverage proves the second append avoids a second size probe and
  still reconstructs the latest page byte-for-byte.
- Production attribution shows fewer standalone-size probes in the
  representative ownerless autocommit sample without payload-byte regression.

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
- The new primitive case appends two same-base index deltas after a compact
  standalone base: the first exact-probes and accepts a retained fast-miss
  payload, while the second fast-accepts from the refreshed estimate. It asserts
  `2` delta records, `1` exact record, `1` fast record, and only `1`
  standalone-size probe, then reconstructs the latest page byte-for-byte.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` preserved `1506` page-log append
  calls and essentially unchanged payload bytes (`556191` versus `556202`
  pre-slice). Fast index deltas increased from `449` to `468`, exact index
  deltas dropped from `32` to `13`, fast standalone-comparison rejections
  dropped from `50` to `29`, standalone-size probe calls dropped from `51` to
  `30`, size-probe time moved from the same-host pre-slice `2.505 ms` sample to
  `1.044 ms`, and exact fallback records reusing fast-miss payloads dropped
  from `44` to `23`.
- A reduced stats-off production sanity probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ownerless autocommit at
  `1457.80 ops/s`, ordinary autocommit at `2850.33 ops/s`, ownerless
  transactional inserts at `1447.42 ops/s`, and ownerless bulk row-list inserts
  at `3435.22 rows/s`. Treat this as sanity evidence; CI runners remain the
  comparable timing source.
- Hook-build focused selectors passed:
  `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$'
  --output-on-failure`.
- `ctest --preset ownerless-stress --output-on-failure` passed all 12 local
  ownerless stress cases before the follow-up formatting-only change; production
  and hook selectors were rerun after formatting.
- `tools/check-ci-production-builds` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `cmake --build --preset format-check-prod` passed with the local
  clang-format runtime library path.
- `git diff --check` passed.

## Risks And Follow-Up

The refreshed estimate is still a heuristic. It may prefer a bounded fast delta
for a later page version whose exact standalone encoding would have become
smaller again. The existing fast payload limit and exact fallback path bound
that tradeoff, and the estimate is volatile. Larger remaining targets are still
native commit/page-publication cost, broader redo/checkpoint reconciliation, and
non-fast page-log encoding that cannot be skipped through this cache.
