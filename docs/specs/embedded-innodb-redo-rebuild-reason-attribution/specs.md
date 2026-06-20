# Embedded InnoDB Redo Rebuild Reason Attribution

## Problem

The InnoDB log-rebuild and recovery-bootstrap attribution slices showed that
ordinary warm open/close timings can be dominated by full redo rebuilds. A
five-iteration reduced production probe reported two actual rebuilds at roughly
`130 ms` each. The current counters show whether rebuilds happened, but not why
MariaDB decided the native redo file had to be rebuilt.

MyLite needs reason-level evidence before optimizing this path. Skipping or
short-circuiting `srv_log_rebuild_if_needed()` without proving the mismatch
would risk native InnoDB redo compatibility and crash recovery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:srv_log_rebuild_if_needed()`
  returns through the no-rebuild path only when:
  - `log_sys.file_size == srv_log_file_size`; and
  - `log_sys.format == (srv_encrypt_log ? log_t::FORMAT_ENC_11 :
    log_t::FORMAT_10_8)`.
- The same function skips rebuild entirely for `SRV_FORCE_NO_LOG_REDO` and
  read-only startup.
- `mariadb/storage/innobase/srv/srv0start.cc:srv_log_rebuild()` flushes dirty
  pages, closes the old redo file, calls `create_log_file(false, lsn)`, and
  then calls `log_sys.resize_rename()`.
- `create_log_file()` writes `ib_logfile101` with `srv_log_file_size`, sets the
  latest format with `log_sys.set_latest_format(srv_encrypt_log)`, and relies on
  `log_sys.resize_rename()` to atomically replace `ib_logfile0`.
- `mariadb/storage/innobase/include/log0log.h` defines `log_t::file_size`,
  `FORMAT_10_8`, `FORMAT_ENC_11`, and `set_latest_format()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` defines the upstream
  `innodb_log_file_size` sysvar default as `96 << 20` with 4096-byte block
  increments.
- `packages/libmylite/src/database.cc:runtime_arguments()` passes
  `--no-defaults`, routes InnoDB data/log/undo/temp paths into the MyLite
  directory layout, and does not currently set `--innodb-log-file-size`.

## Design

Extend the disabled-by-default startup counter table with reason and value
evidence collected immediately before `srv_log_rebuild_if_needed()` calls
`srv_log_rebuild()`:

- size mismatch count;
- format mismatch count;
- observed `log_sys.file_size` accumulated over actual rebuild calls;
- desired `srv_log_file_size` accumulated over actual rebuild calls;
- observed `log_sys.format` accumulated over actual rebuild calls;
- desired redo format accumulated over actual rebuild calls.

Expose the new values from `mylite_embedded_performance_probe` detailed startup
output. Use raw value keys for counts and average value keys for size/format
values, divided by actual rebuild calls. Keep the compact summary focused on
time; reason evidence belongs in the detailed phase output.

This slice does not change the rebuild decision, file creation, rename,
checkpointing, encryption, or crash-recovery order.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
behavior changes. This slice adds internal performance counters and probe
output only.

## Directory And Lifecycle Impact

No directory layout or file lifecycle change. Redo files stay in the native
InnoDB log group directory inside the MyLite database directory.

## Native Storage Impact

No native redo format, size, encryption, checkpoint, or recovery behavior
change. The counters only observe the upstream rebuild predicate.

## Build And Size Impact

The slice adds a few internal counter slots and disabled-by-default arithmetic
when startup attribution is enabled. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced production performance probe and confirm new
  `innodb_log_rebuild_*_mismatch` and observed/desired size/format keys are
  emitted.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output explains whether actual warm-open rebuilds were caused by size
  mismatch, format mismatch, or both.
- Probe output reports observed and desired redo size/format averages for
  rebuild calls.
- Existing log-rebuild timing keys remain available.
- Focused production embedded lifecycle tests pass.
- Docs record the measured rebuild reason and whether an optimization slice can
  target MyLite configuration/lifecycle churn or must leave MariaDB native redo
  reconciliation untouched.

## Verification Results

A reduced five-iteration production probe after adding reason counters reported:

- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_if_needed_calls=5`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_no_rebuild_calls=3`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_calls=2`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_size_mismatch_calls=2`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_format_mismatch_calls=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_observed_file_size_bytes_avg=100663304.000`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_desired_file_size_bytes_avg=100663296.000`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_observed_format_avg=1349024115.000`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_desired_format_avg=1349024115.000`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_total_ms_avg=234.121`.

The measured warm-open rebuilds are caused by redo file-size mismatch only. The
observed native redo file is exactly 8 bytes larger than the configured 96 MiB
`srv_log_file_size`, while the redo format already matches `FORMAT_10_8`. The
follow-up physical-size attribution slice showed that this is not merely an
internal `log_sys.file_size` accounting artifact: the physical `ib_logfile0`
size also averaged `100663304` bytes at rebuild time, physical mismatch calls
matched rebuild calls, and internal/physical divergence was zero. The later
clean-shutdown tail truncation slice keeps the startup rebuild predicate intact
and normalizes the physical tail only after MariaDB's clean shutdown
LSN/checkpoint checks pass.

## Risks And Follow-Up

The reason counters may show that the rebuilds are legitimate MariaDB native
redo reconciliation after creation or recovery. If so, this slice should not be
followed by a skip optimization. If the counters show recurring MyLite-owned
configuration churn, a later optimization can make the startup arguments stable
or persist the intended redo configuration explicitly.
