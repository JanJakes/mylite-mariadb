# Ownerless Checkpoint No-Op Elision

## Problem

Ownerless raw-latest and page-visible checkpoint publication can call the
checkpoint update primitive multiple times for the same latest/visible LSN
pair. After the LSN record slice, `mylite-concurrency.ckpt` has two
checksummed generation records, so readers can detect torn record writes and
do not require every repeated same-pair publication to create another
generation.

Repeated same-pair writes still take the checkpoint byte-range lock, append a
new record generation, update the legacy pair, and may fsync. Prior production
attribution showed checkpoint update cost is not the largest ownerless
autocommit cost, but it remains visible enough to keep separated in the
performance probe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite owns checkpoint LSN publication in
  `packages/libmylite/src/database.cc` through
  `ownerless_persist_redo_checkpoint()`,
  `update_concurrency_checkpoint_lsn_from_redo_state()`,
  `update_concurrency_checkpoint_lsn()`, and
  `write_concurrency_checkpoint_lsn_locked()`.
- Raw-latest redo leave calls publish with `durable=false`; page-visible
  publish and native checkpoint promotion calls publish with `durable=true`.
- The existing page-log sync path uses a process-local sync anchor to elide
  clean page-log syncs only when the same process has already proven the file
  size/header generation are synced.
- MariaDB redo checkpoint recovery still relies on its own checksum-protected
  checkpoint slots in `mariadb/storage/innobase/log/log0recv.cc`; this slice
  changes only MyLite's ownerless checkpoint metadata file.

## Scope And Non-Goals

In scope:

- Detect no-op latest/visible LSN record publications when a valid record
  already stores the same pair.
- Elide the record rewrite for non-durable no-op publications.
- Elide durable no-op publications only when this process has already completed
  a durable checkpoint sync for the same open checkpoint fd and same LSN pair.
- Expose no-op elision counts in the existing ownerless database performance
  counters and production probe summary.
- Prove a no-op checkpoint update does not advance the checksummed generation
  while a later advancing update still publishes and remains recoverable.

Out of scope:

- Group commit for `.ckpt` updates across processes.
- Skipping the first durable sync for a pair written by another process.
- Changing the checkpoint LSN record format.
- Replacing the broader native redo/checkpoint reconciliation design.

## Design

`write_concurrency_checkpoint_lsn_locked()` already reads the highest valid
checkpoint LSN record to choose the next generation. It will now compare that
record to the requested latest/visible pair after normalizing
`visible_lsn <= latest_lsn`.

If the pair matches and the call is non-durable, the function returns success
without writing a new record generation. If the pair matches and the call is
durable, the function returns success without writing only when a
process-local checkpoint sync anchor matches the same open checkpoint fd and
same latest/visible pair. Otherwise it performs the existing record write,
legacy-pair write, and optional sync.

Successful durable writes update the process-local checkpoint sync anchor.
Any advancing or non-durable write that rewrites the record clears that anchor
first, so a later durable publication for the same pair still performs the
required fsync. Closing/releasing an ownerless runtime clears the anchor along
with the existing page-log sync anchor.

The optimization intentionally does not treat a readable record from another
process as durable proof. A cross-process readable record can still be a
non-fsynced page-cache write, so another process's durable same-pair update
must not be skipped.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, or wire-protocol behavior changes. The durable
checkpoint file format is unchanged.

## Directory And Lifecycle Impact

No new file or directory state. The process-local sync anchor is transient and
is reset when the runtime releases ownerless checkpoint state.

## Native Storage Impact

No native InnoDB file, page, or redo format changes. The slice reduces redundant
MyLite checkpoint metadata writes without changing which LSNs are considered
durable or visible.

## Build, Size, License, And Dependencies

No dependency or license impact. Binary-size impact is limited to a small
process-local anchor and one additional performance counter.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Add a focused SQL case proving duplicate same-pair checkpoint writes do not
  advance the LSN record generation, while a later advancing write does.
- Run nearby checkpoint LSN, marker, native checkpoint, and visible-checkpoint
  crash coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Non-durable no-op checkpoint LSN publication does not advance the record
  generation.
- Durable no-op publication is elided only after this process has already
  durably synced the same pair.
- A later advancing checkpoint publication writes a new valid generation.
- Runtime reopen and forced `.shm` rebuild still recover committed ownerless
  data after the no-op elision path.
- Production performance output exposes the no-op elision counter.

## Implementation Evidence

- `write_concurrency_checkpoint_lsn_locked()` skips same-pair non-durable
  record rewrites and skips same-pair durable rewrites only when the
  process-local checkpoint sync anchor matches the same checkpoint fd and LSN
  pair.
- `mylite_ownerless_database_test_update_checkpoint_lsn_repeated()` gives the
  focused SQL harness deterministic access to repeated same-pair checkpoint
  updates without changing public API.
- `test_ownerless_checkpoint_lsn_noop_update_keeps_generation` verifies
  non-durable generation stability, first durable same-pair write plus second
  durable no-op elision, later advancing checkpoint publication, forced
  `.shm` rebuild recovery, and the no-op performance counter.
- The production performance probe emits
  `mylite_perf_summary_ownerless_autocommit_checkpoint_update_noop_elided_per_insert`
  and detailed `*_checkpoint_update_noop_elided` counters.

Verification:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test`
- `cmake --build --preset php-embedded-prod --target mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test checkpoint-lsn-noop-elision`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_checkpoint_lsn_noop_update_keeps_generation`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_checkpoint_lsn_record_recovers_from_torn_latest_slot`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-reclaim`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test visible-checkpoint-crash`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=1 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=1 MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe | rg "checkpoint_update_noop_elided|mylite_perf_summary_ownerless_autocommit_checkpoint_update_noop_elided"`

## Risks And Follow-Up

- Cross-process `.ckpt` group commit and broader checkpoint batching remain
  planned.
- The larger ownerless performance targets remain native commit/page-publication
  cost and broader redo/checkpoint reconciliation.
