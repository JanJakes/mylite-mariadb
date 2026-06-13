# Ownerless Index Delta Base Refresh

## Goal

Keep the non-chained `FIL_PAGE_INDEX` page-log delta format from drifting
against an old standalone base during longer ownerless write runs. The current
format is replay-safe because every delta points at a durable standalone base,
but production probes show that a long same-process insert stream can keep
diffing against the same old base until per-row index delta payload grows.

## Non-Goals

- Do not change the ownerless page-log record format.
- Do not allow delta records to chain through other delta records.
- Do not make replay, checkpoint, or recovery depend on process-local base
  cache state.
- Do not change SQL, C API, mysqli, WordPress, or wire-protocol behavior.
- Do not claim ownerless concurrency complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes committed page images
  after the page LSN and checksum state have been prepared.
- `packages/libmylite/src/ownerless_page_log.cc` owns ownerless page-log
  encoding. The index delta format stores an absolute base-record offset and
  changed byte runs, and `decode_index_delta_payload()` rejects a base that is
  itself a delta record.
- `checkpoint_locked()` and
  `checkpoint_preserving_oldest_snapshot_locked()` rewrite retained delta
  records as standalone records, so compacted logs remain independently
  decodable.
- The current process-local `IndexPageDeltaBaseSlot` records a standalone
  base page after successful standalone append and intentionally does not
  update that base after delta appends. That avoids chained deltas but allows
  long streams to drift against an old base.

## Current Evidence

A reduced stats-enabled production probe on the current branch with
`MYLITE_PERF_INSERT_ITERATIONS=200` reported:

- `mylite_perf_summary_ownerless_insert_autocommit_ops_per_second=1249.65`
  versus ordinary autocommit `2642.75`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=18.519`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_records=201`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_records=189`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_payload_bytes=207878`.

That is about `1039` index-delta payload bytes per insert, worse than the
latest 100-row evidence after the initial delta slice. The issue is cumulative
base drift, not decode correctness.

## Design

Track a bounded `delta_records_since_base` counter in each process-local index
delta base slot.

- A successful standalone index-page append records the page as the current
  durable base and resets `delta_records_since_base`.
- A successful index-delta append increments `delta_records_since_base` for
  the matching base slot.
- `index_delta_base_snapshot()` refuses to supply a base once the slot has
  produced the configured maximum number of delta records. The append path
  then falls back to the already-built standalone payload, and the successful
  standalone append refreshes the base.

The refresh is intentionally a process-local optimization policy. It does not
add durable metadata, change record flags, or change replay requirements. A
process restart, table overflow, checkpoint generation change, or refresh
limit all simply force the next eligible page to be written as a standalone
base.

## Compatibility Impact

No SQL or public API behavior changes. Existing page-log records remain
readable. New records still use the same standalone and index-delta encodings
introduced by the prior slice.

## Directory And Lifecycle Impact

No new files or directory layout changes. Durable state remains in the
existing ownerless page-version WAL inside the MyLite database directory.
The refresh counter is transient process memory and can be discarded at any
time.

## Native Storage Impact

No native InnoDB page, redo, undo, dictionary, or checkpoint format changes.
The ownerless WAL continues to publish byte-exact page images with checksums
computed over the reconstructed full page.

## Build, Size, And Dependencies

No dependency or build-profile change. The implementation adds one small
counter to an existing bounded process-local base-cache slot.

## Test Plan

- Extend ownerless primitive coverage to prove:
  - warm index identities still select delta records;
  - after the bounded delta count, the next append is forced standalone;
  - the forced standalone record is byte-exact and independently readable;
  - a following append can select a new delta against the refreshed base;
  - checkpoint retention still rewrites deltas as standalone records.
- Run focused production ownerless primitive coverage.
- Run a reduced stats-enabled production performance probe and compare
  index-delta payload bytes, index-delta record counts, append encode time,
  and throughput against the current 200-row sample.
- Run focused ownerless SQL selectors, hook/stress coverage, production-build
  guards, formatting, and whitespace checks.

## Acceptance Criteria

- No retained or direct-read delta record depends on process-local memory.
- Forced refresh records are standalone page-log records and read back
  byte-for-byte.
- A refreshed base can seed later delta records without chaining through an
  older delta.
- Production probe evidence shows bounded or reduced index delta payload
  growth over the 200-row sample, or the docs record why the refresh did not
  improve the measured path.

## Implementation Evidence

The implementation adds `delta_records_since_base` to each bounded
process-local index-delta base slot. Successful delta appends increment the
counter; successful standalone appends reset it and become the new durable base.
After `32` delta records from one base, `index_delta_base_snapshot()` refuses
that base so the next eligible record is appended through the existing
standalone encoding path.

`test_page_log_refreshes_index_delta_base()` now proves the refresh boundary:
after one standalone base, the first `32` same-identity records are index
deltas; the next record is forced standalone and reads back byte-for-byte; the
following record can use a new delta against the refreshed standalone base.
The existing `test_page_log_encodes_index_delta_payloads()` still proves
direct reads, latest-page lookup, checkpoint rewrite of retained deltas as
standalone records, and post-checkpoint fallback.

Local production verification on 2026-06-13:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- Focused production ownerless selectors passed:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$'
  --output-on-failure`.
- Direct production ownerless SQL commands passed:
  `native-reclaim`, `live-reclaim`, `commit-race`, and
  `active-reader-pressure`.
- The focused hook subset passed under `ownerless-test-hooks` for ownerless
  primitives, page-write refresh skip, native-support WAL elision, multi-row
  insert visible fast path, stale-drop crash recovery, and the active-reader
  pressure trace.
- The focused stress subset passed under `ownerless-stress` for ownerless
  primitives, history WAL proof, native-support WAL elision, multi-row insert
  visible fast path, DDL stress, active-reader pressure stress, and the
  active-reader pressure trace.
- The reduced 200-row stats-enabled production probe reported
  `mylite_perf_ownerless_insert_autocommit_page_log_append_index_payload_bytes=123855`,
  `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_records=186`,
  and
  `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_payload_bytes=99206`,
  or `619.275` index payload bytes per insert and `496.030` index-delta payload
  bytes per insert. The pre-slice 200-row sample reported `217565` index
  payload bytes and `207878` index-delta payload bytes, so this slice bounded
  drift in the measured long-running shape. The same post-change wall-clock
  sample was noisy, so byte/count counters are the performance evidence.
- A stats-off 200-row production sanity sample reported ownerless single-row
  autocommit at `940.49 ops/s` and ownerless bulk row-list inserts at
  `1763.42 rows/s`; an earlier same-host stats-off bulk sample was noisy and
  lower, so this is sanity evidence rather than the primary optimization
  metric.
- `tools/check-ci-production-builds`, production CMake build-type guards,
  `ctest --preset php-embedded-prod -R '^tools\.ci-production-builds$'
  --output-on-failure`, `cmake --build --preset format-check-prod` with the
  local LLVM library path, and `git diff --check` passed.

## Risks And Follow-Up

Refreshing too often trades small delta records for larger standalone records;
refreshing too rarely lets cumulative diffs grow. This slice uses a conservative
fixed limit to bound drift without adding adaptive heuristics. Broader
workloads may need future tuning, but correctness remains unchanged because
standalone fallback is always valid.
