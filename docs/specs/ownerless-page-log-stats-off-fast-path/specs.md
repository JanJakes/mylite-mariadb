# Ownerless Page-Log Stats-Off Fast Path

## Problem

The remaining ownerless performance gap is concentrated in write-path work, not
process startup or CI build shape. A stats-off production probe at branch head
`c3a8de53` with `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
`MYLITE_PERF_SELECT_ITERATIONS=50`, and `MYLITE_PERF_INSERT_ITERATIONS=1000`
reported ownerless autocommit inserts at `1664.92 ops/s` versus ordinary
autocommit inserts at `3664.68 ops/s`, and ownerless row-list bulk inserts at
`2242.57 rows/s` versus ordinary at `7290.42 rows/s`.

The stats-enabled attribution sample for the same branch showed about three
ownerless page-log appends per autocommit insert. Even when append statistics
are disabled, `packages/libmylite/src/ownerless_page_log.cc` repeatedly checks
the append-stat atomics while appending each record. Those checks do not affect
WAL format or correctness, but they remain on the normal production write path
used by PHP and WordPress tests.

## Source Findings

- MyLite branch head before this slice: `c3a8de53`.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  checks `page_log_append_perf_stats_are_enabled()` around every measured
  subphase: delta snapshot, delta encoding, standalone encoding, payload stats,
  detail page-type stats, checksum, payload write, record-header write, and
  delta-base note.
- `append_at_common()`, `append_locked()`, and
  `mylite_ownerless_page_log_append_session_append()` also create append
  performance scopes and increment counters by calling helpers that reload the
  same disabled stats flag.
- `record_append_payload_encoding_stats()` and
  `record_append_page_type_stats()` already return early when stats are
  disabled. Calling them from the disabled path is unnecessary because their
  results are diagnostics-only.
- The append order remains payload first and record header second; this slice
  does not change crash ordering, record headers, encoded payloads, checksums,
  delta-base retention, checkpointing, or replay.

## Design

Cache the append-stat state at the append entry points and inside
`append_record_at_locked()`:

- add helper overloads for append perf `add` and `add_elapsed` that accept an
  already-computed `stats_enabled` boolean;
- let `PageLogAppendPerfScope` accept a cached enabled flag;
- compute `stats_enabled` once in `append_at_common()`,
  `append_locked()`, append-session begin, append-session append, and append
  session end before touching append counters;
- compute `stats_enabled` and `detail_stats_enabled` once per
  `append_record_at_locked()` call and reuse those flags for subphase timing;
- skip payload composition stats and detail page-type stats entirely when their
  cached flags are disabled.

Stats-enabled behavior remains byte-for-byte and counter-compatible with the
existing diagnostics. Stats-off behavior avoids repeated relaxed atomic loads
and diagnostics-only helper calls while still performing the same WAL append,
checksum, write ordering, and delta-base update.

## Compatibility Impact

No SQL behavior, public C API, PHP API, wire-protocol behavior, WAL format,
page-version record format, checkpoint format, recovery semantics, or native
InnoDB file format changes.

## Directory And Lifecycle Impact

No durable file or directory-layout change. Ownerless page-log records are
written to the same files with the same payload and header ordering.

## Native Storage Impact

No native InnoDB page, redo, undo, purge, checkpoint, or recovery behavior
changes. The current history-proof page images are still published for the
covered ownerless autocommit path; this slice does not elide them.

## Build And Performance Impact

The normal stats-disabled append path performs fewer relaxed atomic loads and
skips diagnostics-only payload/page-type accounting calls. The expected
throughput impact is modest because record count, page checksums, positioned
writes, and history-proof publication remain the larger costs.

No new dependency or generated artifact is added.

## Test And Verification Plan

- Add primitive page-log coverage that appends with stats disabled and asserts
  the append counters stay zero while the record remains readable.
- Run ownerless primitive tests in the production preset.
- Build and run the production performance probe with a reduced stats-off
  sample, then a reduced stats-enabled sample to verify counters still emit.
- Run focused ownerless SQL selectors covering history WAL proof,
  native-support page WAL elision, and multi-row insert visible fast path.
- Run production build guards, CI production-build audit, format check, and
  `git diff --check`.

## Acceptance Criteria

- Stats-off page-log append avoids repeated append-stat flag probes in the hot
  record append path.
- Stats-enabled append counters remain present for direct and session appends.
- Primitive readback proves stats-disabled appends still write valid page-log
  records and keep counters at zero.
- Focused ownerless write-path correctness checks pass with production
  artifacts.

## Verification Results

Local verification on 2026-06-16 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel`; `build/php-embedded-prod`,
`build/ownerless-test-hooks`, and `build/ownerless-stress` were `Release`.

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed after
  adding the stats-disabled append/readback assertion.
- Focused production ownerless SQL selectors passed for
  `history-wal-proof`, `native-support-page-wal-elision`, and
  `multi-row-insert-visible-fast-path`.
- A stats-off production probe with `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` passed. The sample was noisy and did
  not show a clear throughput win: ownerless autocommit was
  `1061.56 ops/s` versus ordinary autocommit at `3400.47 ops/s`, while active
  reconnect stayed in the expected low range at `1.229 ms` for ownerless and
  `0.820 ms` for ordinary.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed and reported
  `604` autocommit page-log appends, `0` direct appends, `200` append-session
  begins, `604` append-session appends, and `200` append-session ends.
- Hook crash selectors `visible-publish-crash` and
  `visible-checkpoint-crash` passed under the `ownerless-test-hooks` build.
- The focused `ownerless-stress` subset passed for ownerless primitives,
  history WAL proof, native-support page WAL elision, and multi-row insert
  visible fast path.
- Production build guards passed:
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/require-cmake-release-build build/ownerless-test-hooks`,
  `tools/require-cmake-release-build build/ownerless-stress`,
  `tools/check-ci-production-builds`, and
  `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Risks And Follow-Up

- This is a bounded CPU-overhead cleanup. It does not remove page-version WAL
  records, replace history-proof pages, or close the full ownerless write
  throughput gap.
- Further high-impact work still needs a correctness proof for broader native
  redo/checkpoint reconciliation or a replacement for the current history-proof
  page images.
