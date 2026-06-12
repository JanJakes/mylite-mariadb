# Ownerless Compact Sparse Payload Composition

## Problem

Ownerless autocommit attribution now reports page-version WAL payload bytes by
InnoDB page class. Current reduced samples show most bytes in rollback-segment
`FIL_PAGE_TYPE_SYS` history-proof pages and user/index pages. The next
optimization choice still needs one more split: whether compact-sparse WAL
payload is dominated by run metadata or by actual nonzero page bytes.

If run metadata dominates, a better sparse representation could help. If
nonzero page bytes dominate, the next optimization needs a proof-level change
that reduces published page images or stores safe deltas against a trusted base.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc::
  build_compact_sparse_zero_payload()` writes compact sparse payloads as a
  16-bit run count followed by `(uint16_t offset, uint16_t size, bytes...)`
  entries.
- `append_record_at_locked()` already owns the encoded payload bytes and the
  original page image before it records append stats and writes the payload.
- `record_append_payload_encoding_stats()` records compact-sparse record counts
  and aggregate payload bytes.
- `record_append_page_type_stats()` classifies each page image into index,
  undo-log, SYS, TRX_SYS, space/allocation, BLOB, or other page classes.
- `packages/libmylite/tests/embedded_performance_probe.c` and
  `packages/libmylite/tests/ownerless_primitives_test.c` mirror the internal
  page-log append stat enum, so any new counters must be appended and mirrored.

## Design

Extend existing stats-only page-log append counters with compact-sparse payload
composition:

- total compact-sparse metadata bytes,
- total compact-sparse nonzero data bytes,
- compact-sparse metadata and data bytes for each existing page-class bucket:
  index, undo-log, SYS, TRX_SYS, space/allocation, BLOB, and other.

The append path derives the composition from the already-built encoded payload
instead of rescanning the page. For compact sparse records, metadata bytes are:

```c
sizeof(uint16_t) + run_count * 2 * sizeof(uint16_t)
```

Data bytes are `payload_size - metadata_bytes`. Malformed internal payloads
are ignored for the new composition counters; normal append validation and
readback behavior remain unchanged.

The embedded performance probe will print raw totals and per-ownerless-insert
summary lines for total compact metadata/data bytes plus the SYS and index
sub-buckets that drive the current performance investigation. Other page-class
composition counters remain available in the raw append dump.

## Scope And Non-Goals

In scope:

- stats-only compact-sparse metadata/data byte attribution,
- primitive coverage for deterministic compact-sparse composition,
- production attribution probe evidence.

Out of scope:

- changing the page-version WAL record format,
- changing checksums, append ordering, or checkpoint behavior,
- replacing history-proof page images,
- delta encoding against native or prior WAL pages.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli behavior, storage-engine behavior, or
directory layout changes. Counters are internal diagnostics read by tests and
the embedded performance probe.

## Directory And Lifecycle Impact

No durable files or record format changes. `concurrency/mylite-concurrency.wal`
continues to store the same compact-sparse bytes.

## Native Storage Impact

No native InnoDB page, redo, undo, checkpoint, or recovery behavior changes.
This slice only observes already-published page-version records.

## Build And Performance Impact

Stats-disabled writes remain unchanged. Stats-enabled appends parse the
compact-sparse run count and add a few relaxed atomic counters for each compact
sparse record. Production CI timing jobs still use Release MyLite and
MinSizeRel MariaDB embedded guards.

## Test And Verification Plan

- Extend ownerless primitive page-log stat assertions for compact-sparse
  metadata/data bytes.
- Extend the embedded performance probe enum mirror and summary output.
- Run focused ownerless primitive coverage.
- Run a reduced production stats-enabled embedded performance probe and record
  compact-sparse composition evidence.
- Run focused ownerless direct SQL selectors for committed-read and commit-race
  sanity.
- Run production build guards, format check, and whitespace check.

## Implementation Evidence

The implementation appends diagnostic counters to the existing page-log append
stat array and mirrors the enum in the ownerless primitive test and embedded
performance probe. It derives composition from the compact sparse payload bytes
that will be written, so stats do not rescan the original page and do not alter
the WAL record format, checksum, append order, or readback path.

The deterministic primitive case uses four one-byte nonzero runs in a 128-byte
page. It asserts one compact sparse record with `22` payload bytes split into
`18` metadata bytes and `4` data bytes, and verifies that legacy 32-bit sparse
fallbacks leave the compact composition counters at zero.

The reduced 100-row production stats-enabled performance probe reported:

- total compact sparse payload: `670161` bytes (`6701.610` per insert),
- compact sparse metadata: `126608` bytes (`1266.080` per insert),
- compact sparse data: `543553` bytes (`5435.530` per insert),
- index compact sparse split: `108622` metadata bytes and `121606` data bytes,
- SYS compact sparse split: `5120` metadata bytes and `412373` data bytes.

The evidence points away from run-metadata compression as the main remaining
simple-insert WAL byte target. The SYS history-proof bucket is dominated by
nonzero data bytes, while the index bucket has a meaningful metadata component
but still contains slightly more nonzero data than metadata in this sample.

## Verification Results

The slice was verified with production embedded builds:

- `cmake --build --preset embedded-prod --target mylite_ownerless_primitives_test mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20 MYLITE_PERF_INSERT_ITERATIONS=100 MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test prepared-committed-read`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test commit-race`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test local-write-first-read`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `tools/require-cmake-release-build build/embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod`
- `git diff --check`

## Acceptance Criteria

- Existing page-log record bytes, readback, checkpoint, and page-type
  attribution remain unchanged.
- Compact-sparse metadata/data counters add up to compact-sparse payload bytes.
- The performance probe identifies whether SYS/index compact-sparse payload is
  mostly metadata or nonzero page data in the reduced ownerless autocommit
  sample.
- Docs record that this is attribution evidence, not an optimization or
  ownerless completion claim.

## Risks And Unresolved Questions

- Counter-only work does not improve throughput by itself.
- The reduced sample may not represent BLOB, compressed-page, or broader DDL
  workloads; those remain covered by separate ownerless matrix and stress
  slices.
