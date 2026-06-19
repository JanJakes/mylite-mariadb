# Ownerless Delta Exact Reuse Probe Cache

## Problem

Ownerless page-log delta selection already has a fast path for bounded small
index, undo, and history deltas. When that fast path rejects a payload only
because it crosses the fast-size limit, exact fallback can still prove the
retained delta is smaller than the current standalone page encoding.

The previous implementation repeated the standalone-size probe for the next
same-base append even after a prior exact fallback had accepted that
fast-miss payload shape. Production attribution showed standalone-size probes
still appearing in the remaining page-log encode cost.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc` stores process-local delta
  base slots keyed by log identity, generation, delta flag, space, page, and
  page size.
- Exact fallback already records negative standalone rejections and can skip a
  later exact probe only for repeated standalone rejections.
- Existing primitive coverage proves fast-limit misses can still become valid
  exact deltas, so a broad skip for all fast-limit misses is unsafe.

## Design

Record positive exact-reuse observations in the existing delta-base slot when:

- the fast path built a rejected delta payload,
- exact fallback probed the current standalone encoded size,
- the rejected fast payload beat that standalone size, and
- the append succeeded as a delta record.

A later append with the same live delta-base slot may reuse one outstanding
positive observation by accepting the already-built rejected fast payload
without running `encoded_payload_size_for_page_probe()`. The skip is capped by
the count of previously observed exact-reuse successes and is reset whenever
the slot is refreshed to a standalone base, invalidated, or replaced.

The WAL format, record flags, checksums, page image reconstruction, checkpoint
rewrite, replay, and page-version visibility rules are unchanged. The cache
only avoids a size-only standalone probe before choosing an already valid delta
encoding.

## Compatibility Impact

No SQL behavior, public API behavior, PHP/mysqli behavior, storage format,
directory layout, or ownerless concurrency ordering changes.

The main tradeoff is WAL size risk: if a later skipped probe would have found a
smaller standalone page, MyLite may append a larger delta record for that page.
Correctness is preserved because both encodings reconstruct the same page and
existing max-delta-record and slot-generation bounds still apply.

## Test Plan

- Build `mylite_ownerless_primitives_test`.
- Run `libmylite.ownerless-primitives`.
- Run focused ownerless SQL cases for history proof, native-support elision,
  and multi-row visible-fast publication.
- Run a reduced stats-enabled production embedded performance probe and inspect
  existing page-log delta/probe counters.
- Run format and whitespace checks before commit.

## Acceptance Criteria

- The first exact fallback reuse still runs the standalone-size probe.
- The next same-base exact fallback reuse can skip that probe while replaying
  the delta record byte-identically.
- The skip budget is one-for-one with earlier positive exact-reuse observations.
- Repeated standalone rejections, build failures, base refresh, and log
  invalidation keep their existing conservative behavior.

## Verification Results

Local verification completed:

```text
cmake --build --preset embedded-prod --target mylite_ownerless_primitives_test
ctest --preset embedded-prod -R '^libmylite\.ownerless-primitives$' --output-on-failure
cmake --build --preset embedded-prod --target mylite_embedded_performance_probe
cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test
ctest --preset embedded-prod -R '^(libmylite\.ownerless-single-owner-history-wal-proof|libmylite\.ownerless-single-owner-native-support-page-wal-elision|libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path)$' --output-on-failure
cmake --build --preset format-check-prod
git diff --check
```

A reduced stats-enabled production probe completed with
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=20`,
`MYLITE_PERF_INSERT_ITERATIONS=200`, and
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. After the final fast-limit guard
tightening, it reported `0.055` exact-reused fast-payload records per
ownerless autocommit insert, `0.080` standalone-size probe calls per insert,
and `0.055` standalone materialization skips per insert. A bulk-oriented
sample with `MYLITE_PERF_INSERT_ITERATIONS=500` and
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100` reported `0.022`
exact-reused fast-payload records, `0.034` standalone-size probe calls, and
`0.022` standalone materialization skips per insert. These representative
short samples did not consume the positive-reuse skip; the primitive test is
the acceptance proof for the repeated fast-limit exact-reuse shape. Treat the
throughput numbers from those short local runs as smoke evidence only.
