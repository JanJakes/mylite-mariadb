# Ownerless Four KiB Delta Fast Path

## Problem Statement

Ownerless page-version WAL appends use non-chained page deltas for repeated
`FIL_PAGE_INDEX`, `FIL_PAGE_UNDO_LOG`, and explicitly marked rollback-segment
history-proof pages. The current fast path accepts deltas up to `2048` bytes
before exact fallback, even when the cached standalone base already proves a
larger delta is still less than half of the standalone payload.

A reduced current-branch production probe with `500` inserted rows and the
default four-row bulk shape reported `41` fast-limit rejections,
`174` standalone-size probe calls, `3.074 ms` in standalone-size probes,
`6.142 ms` in page-log append encode work, and `8.411 ms` total page-log append
time for the ownerless bulk insert phase. This slice raises only the bounded
fast-limit threshold from `2048` to `4096` bytes while preserving the existing
half-standalone-size rule and exact fallback.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc`
  `maybe_encode_page_delta_payload()` builds a non-chained delta against the
  cached standalone base record, rejects fast deltas above
  `k_index_delta_fast_payload_size_limit`, and otherwise still requires
  `index_delta_payload_beats_standalone()` before accepting the delta.
- The same file records accepted delta payloads with existing WAL flags
  (`INDEX_DELTA`, `UNDO_DELTA`, or `HISTORY_RSEG_DELTA`) and rewrites retained
  delta records to standalone records during checkpoint.
- `packages/libmylite/tests/ownerless_primitives_test.c` already covers small
  fast deltas, exact fallback reuse for over-limit deltas, standalone-size
  rejection, checkpoint rewrite, and byte-exact latest reads.

## Scope And Non-Goals

In scope:

- Raise the ownerless page-log fast delta payload cap from `2048` to `4096`
  bytes.
- Prove a medium index-page delta larger than `2048` bytes and no larger than
  `4096` bytes is fast-accepted and reconstructs byte-identically.
- Preserve exact fallback coverage for deltas larger than the new fast cap.
- Record reduced production probe evidence for the same four-row bulk shape.

Out of scope:

- New WAL record formats or flags.
- Chained deltas.
- Changing the half-standalone-size acceptance rule.
- Enabling SYS/TRX_SYS deltas outside explicit history-rseg proof pages.
- Changing page-version visibility, checkpoint rewrite, replay, native
  history-proof publication, or redo/checkpoint recovery.

## Design

The implementation changes only the in-memory fast decision cap:

```c++
constexpr std::uint64_t k_index_delta_fast_payload_size_limit = 4096;
```

The fast path still requires an existing durable standalone base snapshot and
still rejects any delta that does not beat the cached standalone payload by the
existing `delta * 2 < standalone` rule. Deltas above `4096` bytes continue to
use exact fallback, where the current standalone size is probed before the
delta can be accepted.

## Compatibility Impact

No SQL behavior, public C API, PHP/mysqli behavior, wire protocol, durable
directory layout, page-log header, page-log record header, record flags,
checkpoint format, or native MariaDB storage format changes. Existing page-log
read, latest lookup, checkpoint rewrite, and replay readers already understand
the same delta record flags.

## Directory And Lifecycle Impact

No new files, directories, locks, shared-memory fields, or cleanup paths are
added. The slice changes only whether a bounded delta payload uses the fast
decision or exact fallback before appending the same durable page-log record.

## Build, Size, License, And Dependencies

No build-profile, binary-size, license, or dependency changes. The code change
is first-party page-log policy and primitive-test coverage.

## Verification Plan

- Build `mylite_ownerless_primitives_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the focused ownerless primitive CTest.
- Run a reduced production performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- Run adjacent ownerless page-log primitive selectors through the primitive
  test binary.
- Run production-build audit, format check, and `git diff --check`.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- Post-slice reduced production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported the same `383`
  ownerless bulk page-log appends, fast-limit rejections down from `41` to
  `6`, standalone-size probe calls down from `174` to `144`,
  standalone-size probe time down from `3.074 ms` to `1.838 ms`, page-log
  encode time down from `6.142 ms` to `4.977 ms`, page-log append total down
  from `8.411 ms` to `7.183 ms`, and ownerless bulk throughput from
  `4880.07` to `4904.94` rows/s on the same reduced shape.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache)$'
  --output-on-failure` passed.

## Acceptance Criteria

- A medium index-page delta with payload size above `2048` and no larger than
  `4096` bytes is counted as a fast index delta, uses no standalone-size probe,
  and reads back byte-identically.
- A larger index-page delta above `4096` bytes still records a fast-limit miss,
  uses exact fallback, and reads back byte-identically.
- Existing small index deltas, undo deltas, history-rseg deltas, exact
  standalone rejections, checkpoint rewrite, and latest lookup coverage remain
  green.
- Docs keep broader native redo/checkpoint reconciliation, history-proof volume
  reduction, and SYS/TRX_SYS delta reconsideration as separate work.

## Risks And Follow-Up

- A larger fast cap can accept more medium page deltas without recomputing the
  current standalone payload, but it still depends on the cached standalone
  base and half-size proof.
- Larger deltas above `4096` bytes remain on exact fallback until separate
  production evidence proves a higher cap is worth the risk.
- The remaining write-path performance gaps are still native page publication,
  history/native proof volume, and broader redo/checkpoint reconciliation.
