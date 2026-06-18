# Ownerless Delta Base Buffer Reuse

## Problem

The ownerless page-log delta path keeps a process-local standalone base cache so
later index, undo, and explicitly hinted history-rseg records can use bounded
non-chained deltas. The successful append path refreshed that cache by creating
a new `shared_ptr<vector<unsigned char>>` for every standalone base record, even
when the same cache slot already owned an unshared page buffer with enough
capacity. In the current reduced autocommit attribution sample, delta-base note
time is a visible page-log subphase while proof-page identities still churn.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  passes committed, checksum-prepared page images to the MyLite page-version
  hook after the mini-transaction has a commit LSN.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  writes the page-log payload and durable record header before updating the
  volatile delta-base cache.
- `note_index_delta_base_after_successful_append()` is process-local only. Its
  cache contents are hints for later delta eligibility; replay and checkpoint
  correctness come from durable standalone base offsets and full-page checksums
  stored in WAL records.

## Design

Keep the delta-base cache policy and all durable records unchanged. When a
standalone base append refreshes an existing cache slot, reuse the slot's page
vector if no outstanding snapshot holds it. If the vector already has enough
capacity, resize it and copy the new page image into the existing buffer. If a
snapshot is still holding the shared page or allocation fails, fall back to the
existing allocate-or-invalidate behavior.

The append path releases its own delta-base snapshot before noting a standalone
base refresh. That snapshot has already served encoding by then; releasing it
lets the single-owner check distinguish external readers from the append's
temporary reference.

The append perf stats add
`delta_base_page_buffer_reuse_records` so primitive tests and production probes
can show that the reuse branch is active. This counter is diagnostic only.

## Scope And Non-Goals

In scope:

- Process-local delta-base page buffer reuse for standalone base refreshes.
- A primitive assertion for the standalone-refresh branch.
- Production probe output for the new diagnostic counter.

Out of scope:

- Changing page-log record flags, payload encoding, replay, checkpoint rewrite,
  or native history-proof publication.
- Changing delta admission policy for unique undo/history proof pages.
- SQL-level table-lock fault injection, broader DDL/file lifecycle recovery,
  active-reader policy changes, or external MariaDB/RQG stress.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, directory-layout, WAL-format, or native
storage behavior changes. The optimization is limited to a volatile in-process
cache used before future delta appends.

## Test Plan

- Build production targets:
  `mylite_ownerless_primitives_test`,
  `mylite_ownerless_cross_process_sql_test`, and
  `mylite_embedded_performance_probe`.
- Run `libmylite.ownerless-primitives`.
- Run focused ownerless SQL selectors for history WAL proof, native-support
  page WAL elision, visible-fast multi-row insert, and uncommitted peer
  visibility.
- Run a reduced stats-enabled production performance probe and compare
  page-log append shape, delta counts, payload bytes, and delta-base note time.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- Standalone base refreshes reuse the cached page buffer only when the cache
  slot's shared page is unshared.
- Delta records, standalone records, payload bytes, latest-page readback, and
  checkpoint rewrite remain unchanged.
- Primitive coverage observes the diagnostic reuse counter on the
  standalone-refresh path.
- Performance attribution reports the new diagnostic counter.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test
  mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure`
- Reduced stats-enabled production performance probe:
  `/tmp/mylite-perf-delta-base-buffer-reuse-1781804454.log`
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=365`
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_total_ms=12.395`
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_delta_base_note_ms=3.905`
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_delta_base_page_buffer_reuse_records=8`
  - `mylite_perf_summary_ownerless_insert_txn_ratio=0.5843`
  - `mylite_perf_summary_ownerless_insert_autocommit_ratio=0.4035`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`
