# Ownerless No-Dirty Publish Batch

## Problem

The current production ownerless performance probe shows the remaining insert
gap is split across native InnoDB commit work and ownerless page-publication
work. A reduced stats-enabled 200-row production sample at branch head
`99f2576a` reported:

- `mylite_perf_ownerless_insert_autocommit_page_write_commit_log_no_dirty_calls=345`;
- `mylite_perf_ownerless_insert_autocommit_page_write_commit_log_no_dirty_loop_ms=25.964`;
- `mylite_perf_ownerless_insert_autocommit_page_write_publish_calls=800`;
- `mylite_perf_ownerless_insert_autocommit_page_write_publish_hook_ms=32.458`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=602`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_lock_ms=0.741`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_fstat_ms=1.168`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=18.445`.

The append lock and fstat timings are not the dominant cost, but the source
shows inconsistent batching: the made-dirty MTR branch wraps page publication
with `mylite_ownerless_innodb_begin_page_publish_batch()` and
`mylite_ownerless_innodb_end_page_publish_batch()`, while the no-dirty MTR
release loop publishes modified pages directly. That direct path cannot reuse
the append-session lock and file identity established for a batch even when it
publishes multiple ownerless page records.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `99f2576a`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_log()` has two
  relevant ownerless page-publication paths:
  - the made-dirty branch calls `mtr_t::ownerless_page_writes_publish()`,
    which already opens and closes an ownerless page-publish batch around the
    MTR memo scan;
  - the no-dirty branch releases memo slots and may call
    `mtr_t::ownerless_page_write_publish()` directly from inside the release
    loop.
- `packages/libmylite/src/database.cc::append_ownerless_page_version()` uses
  `ownerless_page_log_append_batch` only when a page-publish batch hook has set
  the current hook context. Otherwise it falls back to
  `mylite_ownerless_page_log_append_initialized_at()` for each record.
- `packages/libmylite/src/ownerless_page_log.cc` already exposes process-local
  append timing and byte counters. It does not currently report whether records
  used the direct append path or the append-session path.

## Design

Make the no-dirty MTR publication path use the same page-publish batch
boundaries as the made-dirty MTR scan path. The change is limited to the
ownerless hook-enabled no-dirty branch in `mtr_t::commit_log()` and preserves
the existing order of:

1. release commit-log state,
2. leave ownerless redo,
3. walk and release memo slots,
4. publish modified non-transaction pages as encountered,
5. release page-write latches.

The batch begins immediately before the no-dirty memo loop and ends immediately
after it. The hook is lazy: if no ownerless page-version append occurs, no
append session is opened in first-party code.

Add internal page-log append stats to distinguish:

- direct append calls,
- append-session begin calls,
- append-session append calls,
- append-session end calls.

The stats are appended to the existing private enum so older counter positions
are preserved. The embedded performance probe prints the raw counters under
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, page-log format, native page
format, checkpoint, or recovery semantics change. Page-version records are
written by the same append routine and remain visible only after their valid
record headers are written.

## Directory And Lifecycle Impact

No durable file or directory-layout change. The append session still locks the
same ownerless page-log file descriptor and is closed before returning from the
MTR commit path.

## Native Storage Impact

Native InnoDB MTR ordering, page LSN assignment, redo leave, dirty-page
handling, and memo-slot release order are unchanged. This slice only lets the
existing first-party ownerless append-session helper cover the second MTR
publication branch.

## Build And Performance Impact

Stats-off runtime behavior adds one begin/end hook pair around ownerless
no-dirty MTR release loops. The first-party begin hook is intentionally lazy
and does not acquire the page-log append lock until the first page-version
append. When stats are enabled, the new counters add relaxed atomic increments.

This is a bounded overhead reduction and attribution slice, not a claim that
ownerless insert performance is complete. The current data shows page-log
encoding and native InnoDB write-history/commit work remain larger follow-up
targets than append lock/fstat setup.

## Test Plan

- Update primitive page-log append coverage to verify direct and session
  append counters.
- Build production embedded primitive, performance probe, and ownerless SQL
  targets.
- Run the ownerless primitive suite.
- Run a reduced stats-enabled production performance probe and verify the new
  direct/session append keys are emitted and the simple autocommit page-log
  appends use the session path.
- Run focused ownerless SQL selectors covering history WAL proof,
  native-support page WAL elision, native/live reclaim, commit race, and active
  reader pressure.
- Run ownerless hook and stress focused subsets, production-build guards,
  format-check, and `git diff --check`.

## Acceptance Criteria

- The no-dirty MTR ownerless publication path opens and closes page-publish
  batch hooks around its memo release loop.
- Page-log append stats distinguish direct appends from append-session appends.
- Primitive tests prove the new stats for both append paths.
- Production probe output makes append batching coverage visible.
- Existing ownerless correctness and stress tests pass.

## Verification Results

Local production verification on 2026-06-13:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported:
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=602`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_direct_append_calls=2`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_session_begin_calls=400`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_session_append_calls=600`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_session_end_calls=400`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_lock_ms=0.565`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_fstat_ms=0.561`,
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=18.583`.
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
- `tools/check-ci-production-builds`, production CMake build-type guards,
  `ctest --preset php-embedded-prod -R '^tools\.ci-production-builds$'
  --output-on-failure`, `cmake --build --preset format-check-prod` with the
  local LLVM library path, and `git diff --check` passed.

## Risks And Follow-Up

- The timing benefit is expected to be modest because current append lock/fstat
  subcounters are small relative to page-log encoding and native InnoDB commit
  work.
- Future slices should continue with page-log encoding cost, native
  write-history/commit attribution, and broader redo/checkpoint recovery proof
  rather than eliding history-proof page images blindly.
