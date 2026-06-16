# Ownerless Sparse Payload Direct Copy

## Problem Statement

The ownerless page-log encoder still spends visible CPU in standalone payload
materialization after the recent delta encoder cleanup. A reduced 500-row
stats-enabled production sample at branch head `62246b18` reported `1506`
ownerless autocommit page-log append calls, `24.589ms` in append encoding,
`15.085ms` in standalone encoding, `7.676ms` in delta encoding, `556190`
payload bytes, and stable index/undo delta counts (`481`/`376`).

Standalone compact-varint sparse payloads and fill-sparse raw runs append each
raw byte range with `std::vector::insert()`. The durable bytes are correct, but
the encoder already knows each raw run size and only appends at the end of the
vector, so the range-insert machinery is avoidable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` supplies committed page images to
  MyLite's synchronous page-log append hook; this slice does not change the
  hook boundary.
- `packages/libmylite/src/ownerless_page_log.cc::encoded_payload_size_for_page()`
  chooses standalone trailing-zero, compact sparse, varint compact sparse,
  fill sparse, or full-page payloads before any delta fallback writes the
  record.
- `build_compact_sparse_zero_payload()` writes varuint run descriptors and then
  appends each raw compact-varint run to `varint_payload`.
- `append_fill_sparse_run()` appends raw fill-sparse runs used by both direct
  fill-sparse construction and the SYS/index fill-sparse candidate paths.
- Existing primitive coverage reads back sparse, compact sparse, fill sparse,
  index fill sparse, delta fallback, checkpoint rewrite, and replay payloads
  byte-for-byte.

## Design

Keep the page-log record format, sparse payload layouts, flags, checksums,
checkpoint rewrite behavior, replay behavior, and page-version semantics
unchanged.

For compact-varint sparse payloads, after the run descriptors are written,
resize the vector by the raw run size and copy the run into the appended tail
with `std::memcpy()`.

For fill-sparse raw runs, keep the existing kind byte and descriptor order, but
copy raw bytes into a resized tail with `std::memcpy()` instead of appending the
range with `std::vector::insert()`.

This mirrors the previous delta direct-copy slice for standalone sparse
payloads. It is a CPU-only materialization cleanup; the byte sequence written to
the ownerless WAL is unchanged.

## Compatibility Impact

No SQL behavior, public C API, PHP API, mysqli API, wire-protocol behavior,
storage format, directory layout, or native MariaDB/InnoDB behavior changes.
Existing page-log records continue to decode through the same sparse, fill
sparse, checkpoint, and replay paths.

## Directory And Lifecycle Impact

No new files, shared-memory fields, durable metadata, startup behavior, close
behavior, process ownership rules, or cleanup rules change.

## Native Storage Impact

Native page images, redo, undo, checkpoints, and crash recovery are unchanged.
The slice only changes how MyLite materializes an in-memory standalone sparse
payload buffer before writing the same page-version WAL record.

## Build And Performance Impact

The change removes repeated range-insert calls from compact-varint sparse and
fill-sparse raw-run materialization. The expected effect is a small reduction
in `mylite_perf_*_page_log_append_standalone_encode_ms` without changing append
counts, payload bytes, sparse record counts, delta counts, checksums, or replay
behavior.

## Test And Verification Plan

- Build the production ownerless primitive test, cross-process SQL test, and
  embedded performance probe.
- Run `libmylite.ownerless-primitives` under `php-embedded-prod` to cover
  byte-exact sparse, fill-sparse, index fill-sparse, delta, checkpoint, and
  replay behavior.
- Run a reduced stats-enabled production embedded performance probe and compare
  page-log append counts, payload bytes, sparse record counts, delta counts,
  and standalone encode timing.
- Run focused production ownerless SQL selectors for the history WAL proof,
  native-support page WAL elision, visible-fast insert, and uncommitted peer
  visibility.
- Run focused ownerless hook selectors for primitive and page-publication
  coverage.
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
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` kept `1506` page-log appends,
  `556191` payload bytes, `646` sparse records, `146` varint compact-sparse
  records, `500` fill-sparse records, `481` index delta records, and `376`
  undo delta records for the ownerless autocommit insert phase. The same
  sample reported standalone encode time at `11.168ms`, compared with the
  pre-slice same-shape local sample's `15.085ms`; total append time was
  `36.483ms` compared with `48.625ms`. Throughput samples remain noisy because
  the probe includes embedded runtime and native InnoDB work; the invariant
  checks are stable payload shape and unchanged WAL counts.
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
- Full ownerless stress passed:
  `timeout 900s ctest --preset ownerless-stress --output-on-failure`, 12/12
  tests in `299.48s`.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Existing page-log primitive tests pass unchanged.
- Reduced production attribution still reports stable page-log append counts,
  sparse record counts, delta counts, and payload bytes for ownerless
  autocommit inserts.
- Sparse and fill-sparse readback, replay, and checkpoint rewrite continue to
  return byte-identical page images.
- No checkpoint, recovery, SQL, public API, or directory-lifecycle behavior
  changes are introduced.

## Risks And Follow-Up

This is a bounded CPU cleanup, not the whole ownerless performance answer.
Native commit/page-publication cost, remaining page-version volume, payload
write cost, and broader redo/checkpoint reconciliation remain higher-impact
ownerless performance targets.
