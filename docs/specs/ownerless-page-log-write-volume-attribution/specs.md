# Ownerless Page-Log Write-Volume Attribution

## Problem

Production ownerless insert probes still show a material gap against the
ordinary embedded path. The latest reduced stats-enabled production sample
shows page-log append and MTR page-publish time remain part of that gap, but
the append probe only reports time. Before changing page-version WAL format,
native-support publication, or redo/checkpoint handoff policy, the probe needs
to report how many durable page-log bytes each measured ownerless insert writes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `3c697e2e`.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  writes the page payload first, then writes the valid record header last.
  Readers discover only complete records, so this ordering is part of the
  crash-tail proof.
- `mylite_ownerless_page_log_append_session_append()` reuses
  `append_record_at_locked()`, so one byte counter in that helper covers both
  per-record appends and batched MTR publish appends.
- `packages/libmylite/tests/embedded_performance_probe.c` already mirrors the
  internal append timing stat enum and emits stats only when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- Recent ownerless history and native-support audits found that eliding the two
  remaining history proof page-version records is not correctness-safe without
  broader native redo/checkpoint proof. This slice therefore observes write
  volume instead of reducing it.

## Design

Append two internal page-log append stats after the existing timing counters:

- successfully written payload bytes,
- successfully written record-header bytes.

The counters increment only after their matching write succeeds. Failed writes
therefore do not inflate the reported durable append volume. The existing stat
indexes are preserved by appending the new counters at the end of the enum.

The embedded production performance probe prints raw totals:

- `*_page_log_append_payload_bytes`,
- `*_page_log_append_record_header_bytes`,
- `*_page_log_append_total_record_bytes`.

The stats-enabled ownerless autocommit summary also prints per-insert averages:

- `mylite_perf_summary_ownerless_autocommit_page_log_payload_bytes_per_insert`,
- `mylite_perf_summary_ownerless_autocommit_page_log_record_header_bytes_per_insert`,
- `mylite_perf_summary_ownerless_autocommit_page_log_total_record_bytes_per_insert`.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, storage-format, or directory-layout
changes. The new counters are internal diagnostic hooks used by tests and the
production performance probe.

## Directory And Lifecycle Impact

No new durable files or metadata. Counters are process-local atomics and are
reset by the performance probe between measured ownerless insert phases.

## Native Storage Impact

No native InnoDB page, redo, undo, data dictionary, or checkpoint behavior
changes. The page-version WAL still writes payload bytes before the valid
record header and uses the existing checksum and replay policy.

## Build And Performance Impact

Default runtime overhead remains the existing relaxed boolean check around
append stats. When stats are enabled, each successful page-log append performs
two extra relaxed atomic additions. CI timing jobs use production build guards,
so the reported byte and time samples are from `Release` MyLite and
`MinSizeRel` MariaDB embedded artifacts.

## Test Plan

- Rebuild production embedded test/probe targets.
- Run the ownerless primitive test to prove direct and session appends report
  deterministic payload and record-header byte totals.
- Run a reduced stats-enabled production embedded performance probe and verify
  the new raw and summary keys are present.
- Run focused ownerless SQL selectors that exercise history WAL proof and
  native-support publication.
- Run the CI production-build audit, format-check, and `git diff --check`.

## Acceptance Criteria

- Existing append timing stat indexes continue to report the same timings.
- Byte counters are append-only stats and count successful writes only.
- Primitive coverage verifies direct and batched append write volume.
- Production probe output shows raw and per-insert ownerless page-log byte
  volume.
- Documentation records that this is attribution evidence, not a throughput
  optimization or a completion claim for ownerless concurrency.

## Verification

- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20
  MYLITE_PERF_INSERT_ITERATIONS=300
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  reported:
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_payload_bytes=15024128`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_record_header_bytes=58688`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_total_record_bytes=15082816`,
  - `mylite_perf_summary_ownerless_autocommit_page_log_payload_bytes_per_insert=50080.427`,
  - `mylite_perf_summary_ownerless_autocommit_page_log_record_header_bytes_per_insert=195.627`,
  - `mylite_perf_summary_ownerless_autocommit_page_log_total_record_bytes_per_insert=50276.053`.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  local-write-first-read`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-single-owner-native-support-page-wal-elision$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `tools/require-cmake-release-build build/prod`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/ownerless-test-hooks`
- `tools/require-cmake-release-build build/wordpress-php-embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/wordpress-mariadb-embedded`
- `git diff --check`
