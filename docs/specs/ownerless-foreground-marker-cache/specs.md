# Ownerless Foreground Reclaim Hot Path

## Problem

The production append-fast-path probe shows that single-owner ownerless
autocommit inserts still spend measurable time in
`prepared_step_reclaim_ms` after the proof-pair append path has made page-log
publication cheap. A fresh 200-insert sample on this branch reported:

- ordinary autocommit insert throughput: `3550.02 ops/s`;
- ownerless autocommit insert throughput: `1405.03 ops/s`;
- ownerless `prepared_step_reclaim_ms`: `41.276 ms`;
- history proof pairs: `200/200` succeeded;
- page-log append: `0.078 ms/insert`.

The foreground reclaim function no longer scans WAL in this path, but it still
does hot-path bookkeeping before it can prove reclaim is unnecessary:

- it checks WAL bytes with `fstat()` on each statement boundary;
- it asks whether the native file-operation checkpoint marker is pending by
  taking a checkpoint-file byte-range lock and reading the marker records once
  WAL crosses the normal 64 KiB threshold;
- it wakes the checkpoint scheduler at every statement end even though the
  scheduler already waits for the same 50 ms idle interval before reclaiming.

In a single-owner DML loop, no other live process can introduce the native
file-operation marker. Ordinary DML can only make it pending through the local
InnoDB file-operation redo hook, which already flows through MyLite marker
writers. Page-log appends also know the exact next record offset before
statement reclaim runs, so foreground reclaim does not need a fresh `fstat()`
while the known end offset is still below the 64 KiB statement threshold.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `maybe_reclaim_ownerless_page_log_after_statement()` calls
  `ownerless_single_owner_foreground_reclaim_budget_skips()` for ownerless
  writes whose page-version WAL crossed the normal 64 KiB statement checkpoint
  threshold.
- `read_concurrency_native_file_op_checkpoint_needed()` takes the checkpoint
  file lock, reads both durable marker slots, and falls back to the legacy
  marker word.
- `mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl()`,
  `mark_ownerless_native_file_op_checkpoint_after_successful_write()`, and
  `mark_ownerless_native_file_op_checkpoint_before_dictionary_finish()` are the
  MyLite runtime paths that persist a local pending native file-operation
  marker.
- `reclaim_ownerless_page_log_after_native_checkpoint()` and
  `clear_ownerless_native_file_op_checkpoint_without_page_log()` are the paths
  that clear the marker after native checkpoint proof.
- `packages/libmylite/src/ownerless_page_log.cc`
  `append_record_at_locked()` computes the exact `end_offset` for each appended
  record and already returns it to session appends through
  `mylite_ownerless_page_log_append_session::next_record_offset`.
- `ownerless_checkpoint_scheduler_loop()` wakes periodically using
  `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS` and then rejects reclaim
  until no statement is active and the runtime has been idle for the same
  interval.
- `mariadb/storage/innobase/fil/fil0fil.cc` records InnoDB native file
  operation redo through `mylite_ownerless_innodb_note_file_op_redo()`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  stores that file-operation redo flag in process-local atomics and exposes it
  through `mylite_ownerless_innodb_take_file_op_redo()`.

## Design

Cache foreground reclaim evidence in `RuntimeState`.

First, cache the native file-operation checkpoint marker state.

The persisted checkpoint file remains authoritative at lifecycle boundaries:

- ownerless runtime open reads the marker and initializes the cache;
- any failed marker read invalidates the cache and makes the foreground budget
  skip fall back to the safe path;
- successful marker writes set the cache to pending;
- marker write failures also leave the cache pending for foreground-reclaim
  purposes, because the InnoDB file-operation redo bit is still pending in
  process-local state;
- successful marker clears set the cache to not pending;
- reclaim paths that already read the marker keep reading the durable file and
  refresh the cache from that result.

`ownerless_single_owner_foreground_reclaim_budget_skips()` may use the cached
not-pending value only after the existing single-owner epoch and 64 MiB
foreground-budget predicates pass. If the process is no longer single-owner,
or the cache is invalid, it keeps the previous conservative behavior and does
not claim the cheap skip without durable evidence.

The slice also adds ownerless database performance counters for the foreground
budget skip:

- calls;
- allowed skips;
- marker-cache hits;
- marker-file reads;
- marker-pending blocks;
- marker-read blocks.

These counters let the existing performance probe and focused SQL test prove
that steady-state DML uses the cache while marker-pending cases remain
observable.

Second, cache the known ownerless page-log end offset.

- Runtime open seeds `ownerless_page_log_known_end_offset` from the WAL file
  size.
- Page-log append wrappers expose exact next-record offsets for direct append
  calls that previously returned only the record offset.
- Session appends reuse the already maintained
  `mylite_ownerless_page_log_append_session::next_record_offset`.
- Successful page-log appends monotonically advance the runtime's atomic known
  end offset.
- Native checkpoint reclaim refreshes the known end offset from the file after
  checkpointing, because checkpoint may truncate or retain a smaller suffix.

`maybe_reclaim_ownerless_page_log_after_statement()` now uses the known end
offset before taking `g_runtime.mutex`. If the known payload bytes are below
`MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES`, the statement returns from
foreground reclaim without a mutex acquisition or WAL `fstat()`. The local-write
flag is now atomic so this pre-lock reject still preserves no-live/native
reclaim safety evidence.

Third, avoid waking the checkpoint scheduler at every statement end. The
scheduler still wakes periodically and close still notifies it for shutdown.
Explicit transaction state changes still notify because they are infrequent and
can unblock reclaim policy. Statement-end wakeups were redundant for tight DML
loops because the scheduler cannot reclaim until the idle interval elapses.

## Non-Goals

- No change to WAL format, checkpoint file format, or recovery record layout.
- No change to native checkpoint proof.
- No change to the timer interval, close-time reclaim, or live-peer reclaim
  proof.
- No claim that SQL-level table-lock fault injection is reachable.
- No change to public C API or directory layout.

## Compatibility Impact

SQL behavior and MySQL/MariaDB compatibility are unchanged. The caches only
remove repeated foreground-reclaim bookkeeping from the single-process,
single-owner ownerless DML hot path where no peer can concurrently set the
marker and page-log append already produced a newer exact end offset.

## Directory And Lifecycle Impact

No durable files are added. The caches are process-local and discarded on
runtime cleanup. Durable marker state remains in the MyLite database directory
checkpoint file and is read again on each ownerless runtime open. The known
page-log end offset is only an optimization; durable WAL size remains the
authority when the cache is absent or when reclaim/checkpointing refreshes it.

## Native Storage Impact

Native InnoDB file-operation redo remains authoritative. InnoDB still records
file operations through the MyLite ownerless hook, and MyLite still persists the
checkpoint marker when a native checkpoint proof cannot clear the condition
immediately.

## Build And Performance Impact

The common single-owner foreground path avoids:

- a checkpoint-file `fcntl()` lock and marker read after the runtime has read a
  clean marker at open;
- the runtime mutex and WAL `fstat()` while the known page-log payload is below
  the normal statement checkpoint threshold;
- scheduler wakeups on every statement boundary.

The performance probe should show foreground reclaim budget marker-cache hits
and no marker file reads during the measured single-owner autocommit insert
loop. The probe also records post-slice prepared-step timing evidence, but the
remaining ownerless/ordinary gap is still dominated by native DML/commit
publication and local samples are noisy.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` with the `php-embedded-prod`
  preset.
- Run the focused ownerless foreground budget selectors:
  `single-owner-foreground-reclaim-budget` and
  `single-owner-foreground-reclaim-peer-history`.
- Run focused native marker selectors:
  `native-file-op-marker-drain`, `native-reclaim`,
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`, and
  `live-reclaim`.
- Run a reduced production append-fast-path probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`, and
  `MYLITE_PERF_OWNERLESS_APPEND_STATS=1`.
- Run production primitive and format checks needed for ownerless slices.

## Acceptance Criteria

- The single-owner foreground budget test observes marker-cache hits and zero
  marker-file reads for the steady-state DML statement.
- Pending marker tests still persist and clear the native file-operation marker
  through the durable checkpoint file.
- Below-threshold foreground reclaim can return before taking the runtime mutex
  or calling `fstat()` when append-side known end-offset evidence is current.
- Timer-driven and close-time reclaim still checkpoint retained WAL.
- The append-fast-path probe keeps history-proof pair publication active,
  proves foreground marker-cache use, and records timing evidence for the
  remaining ownerless autocommit gap.
- Focused ownerless SQL selectors, primitive coverage, format check, and
  `git diff --check` pass.

## Verification Results

Local production verification on 2026-06-22:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-foreground-reclaim-(budget|peer-history)$'
  --output-on-failure` passed.
- `ctest --preset php-embedded-prod -L compat.ownerless-primitives
  --output-on-failure` passed.
- Direct ownerless SQL selectors passed:
  `native-file-op-marker-drain`, `native-reclaim`,
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`, and
  `live-reclaim`.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `ctest --preset php-embedded-prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

Reduced append-path probe command:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=20 \
MYLITE_PERF_INSERT_ITERATIONS=200 \
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100 \
MYLITE_PERF_OWNERLESS_APPEND_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The fresh pre-slice attribution sample reported ordinary autocommit insert
throughput at `3550.02 ops/s`, ownerless autocommit insert throughput at
`1405.03 ops/s`, ownerless prepared-step total time at `142.110 ms`, MySQL
execute time at `91.182 ms`, and foreground reclaim time at `41.276 ms`.

The best clean post-slice sample reported ordinary autocommit insert throughput
at `3518.20 ops/s`, ownerless autocommit insert throughput at `1462.08 ops/s`,
ownerless prepared-step total time at `136.546 ms`, MySQL execute time at
`93.672 ms`, and foreground reclaim time at `36.309 ms`.

A final post-format sample remained within the observed noisy range: ordinary
autocommit insert throughput at `3376.59 ops/s`, ownerless autocommit insert
throughput at `1302.28 ops/s`, ownerless prepared-step total time at
`153.279 ms`, MySQL execute time at `97.383 ms`, and foreground reclaim time at
`48.631 ms`. It still proved the intended hot-path evidence:
`200/200` history proof pairs succeeded, foreground reclaim saw two
marker-cache hits, and foreground marker-file reads stayed at zero.

The longer 2048-insert stats-off probe improved from the earlier branch sample
of ordinary `2906.34 ops/s` and ownerless `683.50 ops/s` to ordinary
`3300.29 ops/s` and ownerless `777.22 ops/s`. This slice therefore removes
specific redundant foreground bookkeeping, but it does not close the remaining
ownerless autocommit performance gap.

## Risks And Follow-Up

The cache is intentionally scoped to the owning process and is only trusted
after the existing single-owner predicate passes. Broader active-reader
pressure policy, DDL/file lifecycle recovery classes, and external MariaDB/RQG
stress remain outside this slice.
