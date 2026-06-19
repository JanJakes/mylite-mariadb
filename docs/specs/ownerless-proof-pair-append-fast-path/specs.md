# Ownerless Proof-Pair Append Fast Path

## Problem

Ownerless history-proof publication writes two native-support proof-only
page-log records for each eligible rollback-segment/undo proof pair. The
proof-only WAL slice removed the 16 KiB page payloads, but the paired hook still
called the generic page-log append path twice. That path revalidates ordinary
page append options and enters the page/delta encoding scaffold even though the
records are known native-support proof metadata with zero payload.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` marks the active history-proof
  rollback-segment and undo pages before the history mini-transaction commits.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_history_proof_publish_pair()` gathers the two proof pages
  from the MTR memo after `m_commit_lsn` is known, rejects unsafe/stats/fault
  cases, and calls `mylite_ownerless_innodb_publish_history_proof_pair()`.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_history_proof_publish_pair_hook()` batches the two records
  through the current thread's page-log append session when available and falls
  back closed when test faults or incompatible active batches are present.
- `packages/libmylite/src/ownerless_page_log.cc`
  `append_record_at_locked()` stores proof-only records as ordinary 64-byte
  page-log headers with `NATIVE_SUPPORT_STATE` and `PROOF_ONLY`, zero payload
  bytes, and zero payload checksum. Latest scans, direct reads, replay, and
  checkpoint retained callbacks already treat proof-only records as metadata,
  not page images.

## Design

Add a page-log append-session API for exactly two native-support proof-only
records:

- keep the existing WAL record format;
- write two independent 64-byte record headers in the same order as the old
  two-call path;
- advance the append session after the first header before writing the second,
  preserving the old partial-pair crash shape;
- preserve append-call and session-append counters as record counts;
- skip generic payload, checksum, page-type, and delta-encoding work that is
  impossible for proof-only records.

`ownerless_innodb_history_proof_publish_pair_hook()` uses the specialized pair
append only when the normal page-log append session is active. If the session
cannot be opened, it falls back to the existing generic two-append path. Unsafe
ownerless hook builds and named-fault runs keep the existing unavailable/error
behavior.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, DDL, or storage-engine behavior changes. The
same two proof-only WAL records remain durable and retained by the same
checkpoint rules. The change is internal ownerless bookkeeping.

## Directory And Lifecycle Impact

No new files, directory-layout changes, or persistent format changes. The
records written by the fast path are byte-compatible with existing proof-only
records on this branch.

## Native Storage Impact

Native MariaDB/InnoDB files and page formats are unchanged. The slice does not
relax `trx_t::write_serialisation_history()` proof requirements, native redo
ordering, or checkpoint publication.

## Performance Impact

The targeted reduction is per-pair CPU work in production stats-off ownerless
history-proof publication. Stats-enabled page-publish probes intentionally
disable `mtr_t::ownerless_history_proof_publish_pair()` so they still exercise
the conservative per-page instrumentation path; they remain useful for stable
record-volume checks, not for measuring this fast path.

A reduced stats-off production probe over `5000` inserts with `100` rows per
bulk statement reported ownerless bulk at `24520.62 rows/s`, ordinary bulk at
`81550.84 rows/s`, and an ownerless/ordinary bulk ratio of `0.3007`. This is a
single local sample in the recent noisy range, so it is evidence for a bounded
append-path cleanup rather than closure of the broader ownerless throughput
gap.

## Tests And Verification Plan

- Add primitive page-log coverage for the pair API: two proof-only
  native-support records, adjacent zero-payload offsets, preserved append
  counters, latest/read rejection, replay skipping, and checkpoint
  retained-callback skipping.
- Run focused ownerless SQL selectors for history proof, native-support WAL
  elision, visible-fast multi-row inserts, and uncommitted peer visibility.
- Run unsafe hook selectors covering history-proof failure and visible publish
  crash windows.
- Run ownerless stress and production build/static checks.

## Acceptance Criteria

- The fast path writes the same two proof-only native-support records and no
  page-image payloads.
- Generic fallback remains available when append-session setup is unavailable.
- Existing history-proof SQL coverage still proves successful pair publication
  and zero fallback native history flush pages in normal builds.
- Focused primitive, SQL, hook, stress, format, and diff checks pass.
- Documentation continues to mark broader native redo/checkpoint, DDL/file
  lifecycle recovery, SQL-level table-wait coverage, and external stress as
  planned rather than complete.

## Verification

- `cmake --build --preset embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test
  mylite_embedded_performance_probe` passed.
- `ctest --preset embedded-prod -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure` passed after formatting.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test`
  passed after formatting.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-primitives|ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path))$'
  --output-on-failure` passed after formatting.
- Direct unsafe hook selectors passed after formatting:
  `history-proof-publish-failure-fallback`, `visible-publish-crash`,
  `visible-checkpoint-crash`, and `page-publish-before-append-crash`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed after formatting.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`
  passed after formatting.
- A reduced stats-enabled production probe with page-publish/page-log stats
  preserved `2.000` ownerless autocommit page versions per insert,
  `2.000` native-support proof publications per insert, zero commit-visibility
  flushes, `3.040` page-log append calls per insert, and bulk
  `2.000` page versions, `2.000` native-support proof publications, and
  `8.000` page-log append calls per statement. As expected for that
  instrumentation mode, `history_proof_pair_calls` was `0` because the
  page-publish stats path disables the pair hook.
- A reduced stats-off production probe with `5000` inserts and
  `100` rows per bulk statement reported ownerless bulk at
  `24520.62 rows/s`, ordinary bulk at `81550.84 rows/s`, and an
  ownerless/ordinary ratio of `0.3007`.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/embedded-prod`,
  `tools/require-cmake-release-build build/ownerless-test-hooks`, and
  `tools/require-cmake-release-build build/ownerless-stress` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.
