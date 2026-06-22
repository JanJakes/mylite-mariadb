# Embedded InnoDB Redo Log Size Profile

## Problem

After redo-tail truncation and temporary rollback-segment pool sizing removed
two startup spikes, reduced production probes still showed clean InnoDB redo
scanning as a leading warm-open cost. A two-iteration production sample on
2026-06-22 reported zero actual redo rebuilds, but ordinary warm opens still
spent `45.391 ms` in `recv_recovery_from_checkpoint_start()`, with
`36.795 ms` in the initial clean scan and `8.584 ms` in the clean rescan.
Ownerless warm opens spent `32.755 ms` in the same recovery-start bucket, with
`28.074 ms` in the initial scan and `4.668 ms` in the rescan.

The previous attribution slices showed that MyLite inherited MariaDB's daemon
default `innodb_log_file_size` of 96 MiB. MyLite's process-style embedded
profile pays that clean scan cost on repeated open/close cycles, including PHP
test workloads that create many short-lived database processes or handles.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` defines
  `innodb_log_file_size` as a command-line system variable backed by
  `srv_log_file_size`, with an upstream default of `96 << 20`, a minimum of
  `4 << 20`, and 4096-byte increments.
- The same source defines the read-only startup `innodb_log_buffer_size`
  default as `16U << 20`. The dynamic `innodb_log_file_size` update path
  rejects values smaller than `log_sys.buf_size`.
- `packages/libmylite/src/database.cc:runtime_arguments()` owns the embedded
  MariaDB argument vector and already routes InnoDB data, redo, undo, and
  temporary paths into the MyLite database directory layout.
- `mariadb/storage/innobase/srv/srv0start.cc:srv_start()` recovers or validates
  the existing redo log before `srv_log_rebuild_if_needed()` reconciles size or
  format mismatches, so changing the configured size uses MariaDB's native
  recovery and rebuild paths instead of bypassing redo validation.

## Design

Set MyLite's embedded runtime argument
`--innodb-log-file-size=16777216`.

This gives the embedded profile a 16 MiB native redo file:

- small enough to reduce repeated clean-scan work compared with the inherited
  96 MiB daemon default;
- not smaller than MariaDB's default 16 MiB redo log buffer; and
- still inside MariaDB's native redo format, checkpoint, recovery, and rebuild
  machinery.

The slice does not add a public configuration knob. It keeps redo in
`datadir/ib_logfile0`, keeps the existing clean-shutdown tail truncation
guards, and leaves `srv_log_rebuild_if_needed()` unchanged. Existing database
directories with a 96 MiB redo file may pay a one-time native redo resize or
rebuild on first read/write open after the profile change. Read-only opens do
not rewrite native redo and may continue attaching the previous physical redo
size until a read/write open performs MariaDB's native reconciliation.

## Compatibility Impact

SQL behavior, public C API behavior, PHP mysqli behavior, wire-protocol
behavior, and native redo format are unchanged. The observable tradeoff is a
smaller embedded redo capacity, which can increase checkpoint pressure for very
large write bursts compared with upstream MariaDB's daemon default. That is a
profile choice for the short-lived embedded workload; broader write-throughput
evidence can justify a later configurable profile.

## Directory And Lifecycle Impact

No directory names or durable file ownership change. The native redo file
remains `datadir/ib_logfile0` inside the MyLite database directory. Clean close
still leaves the redo file at the configured size, and ownerless final clean
shutdown still uses the same configured-size invariant.

## Native Storage Impact

InnoDB still owns redo creation, checkpoint validation, crash recovery, and
size reconciliation. The slice changes only the `srv_log_file_size` value
provided to native startup.

## Build And Size Impact

The slice changes one first-party runtime argument and one focused lifecycle
test. It adds no dependency and has no meaningful binary-size impact.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe`,
  `mylite_embedded_open_close_test`, and
  `mylite_embedded_transactions_recovery_test` with `php-embedded-prod`.
- Run focused embedded open/close lifecycle coverage, including the direct
  assertion that `@@innodb_log_file_size` and final `ib_logfile0` size equal
  `16777216`.
- Run focused embedded transaction recovery coverage.
- Run a reduced production performance probe and compare clean redo scan time
  with the pre-change sample.
- Run ownerless hook/stress coverage that exercises ownerless startup,
  shutdown, and retained native recovery state.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Embedded startup reports `@@innodb_log_file_size = 16777216`.
- Clean close leaves `datadir/ib_logfile0` at `16777216` bytes.
- Ordinary and ownerless warm-open samples complete without actual redo
  rebuilds caused by the new steady-state configured size.
- Reduced production probe shows lower clean redo scan time than the 96 MiB
  before sample.
- Focused lifecycle, recovery, ownerless, production-build, format, and diff
  checks pass.

## Verification Results

The slice is verified with:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_embedded_open_close_test
  mylite_embedded_transactions_recovery_test
  mylite_ownerless_cross_process_sql_test`;
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-open-close$' --output-on-failure`;
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-exec$' --output-on-failure`;
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-transactions-recovery$' --output-on-failure`;
- `ctest --preset php-embedded-prod -R
  'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace'
  --output-on-failure`;
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  temp-stress`;
- reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2 MYLITE_PERF_SELECT_ITERATIONS=1
  MYLITE_PERF_INSERT_ITERATIONS=1
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`;
- production build guards, format check, and `git diff --check`.

The focused open/close lifecycle test asserts both
`@@innodb_log_file_size = 16777216` and final `datadir/ib_logfile0` size
`16777216`.

The reduced production probe reported zero ordinary and ownerless warm-open
redo rebuilds after the size change. Compared with the immediately preceding
two-iteration 96 MiB sample:

- ordinary `startup_innodb_srv_start_total_ms_avg` changed from `72.231 ms` to
  `38.538 ms`;
- ordinary `startup_innodb_recovery_start_ms_avg` changed from `45.391 ms` to
  `23.570 ms`;
- ordinary initial clean scan changed from `36.795 ms` to `19.208 ms`;
- ordinary clean rescan changed from `8.584 ms` to `4.351 ms`;
- ownerless `startup_innodb_srv_start_total_ms_avg` changed from `48.987 ms`
  to `30.786 ms`;
- ownerless `startup_innodb_recovery_start_ms_avg` changed from `32.755 ms` to
  `20.636 ms`;
- ownerless initial clean scan changed from `28.074 ms` to `16.809 ms`; and
- ownerless clean rescan changed from `4.668 ms` to `3.815 ms`.

Overall warm open/close remains noisy in tiny reduced samples, so the
acceptance evidence is the native startup subphase reduction plus focused
lifecycle and recovery coverage.

## Risks And Follow-Up

- Existing databases with larger redo logs may pay a one-time native resize or
  rebuild on first open after this profile change.
- The smaller redo file can increase checkpoint pressure for write-heavy
  workloads. The current slice optimizes process-style startup cost; broad
  write-throughput and external stress evidence remain follow-up work.
- This does not remove all clean redo scanning. Any future fast path must prove
  checkpoint and crash-recovery safety against MariaDB's native redo logic.
