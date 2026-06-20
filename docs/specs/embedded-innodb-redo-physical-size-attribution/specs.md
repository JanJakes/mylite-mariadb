# Embedded InnoDB Redo Physical-Size Attribution

## Problem

The redo rebuild reason-attribution slice showed that reduced production
warm-open probes can spend hundreds of milliseconds rebuilding the native InnoDB
redo file. The measured rebuilds were caused by size mismatch only:
`log_sys.file_size` averaged `100663304` bytes while configured
`srv_log_file_size` was `100663296` bytes, and the observed redo format matched
MariaDB's desired `FORMAT_10_8`.

The remaining ambiguity is whether the extra 8 bytes are the physical
`ib_logfile0` size at the rebuild decision or an internal logical size retained
after checkpoint discovery. MyLite needs that distinction before optimizing the
slow path. If the physical file really grew, the next fix belongs near shutdown
or redo truncation. If the physical file is already the configured size, the
next fix can target the startup decision or internal size normalization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0recv.cc:recv_sys_t::find_checkpoint()`
  opens `ib_logfile0`, reads the physical size with `os_file_get_size(file)`,
  and calls `log_sys.attach(file, size)`.
- `mariadb/storage/innobase/log/log0log.cc:log_t::attach()` stores that
  supplied size in `log_sys.file_size` before memory mapping or allocating redo
  buffers.
- `mariadb/storage/innobase/srv/srv0start.cc:srv_log_rebuild_if_needed()`
  currently treats `log_sys.file_size != srv_log_file_size` as an actual resize
  reason before calling `srv_log_rebuild()`.
- `mariadb/storage/innobase/srv/srv0start.cc:create_log_file()` creates
  `ib_logfile101`, sets it to exactly `srv_log_file_size`, attaches it with the
  same size, writes a checkpoint, and later `log_t::resize_rename()` replaces
  `ib_logfile0`.
- `mariadb/storage/innobase/include/os0file.h:os_file_get_size(const char *)`
  reports both total and allocated file size through `stat(2)`, which can be
  sampled by filename without reaching into `log_file_t` internals.

## Design

Extend the disabled-by-default startup counter table with physical redo size
evidence collected in `srv_log_rebuild_if_needed()` immediately before any
actual rebuild call:

- physical size sample count;
- physical size stat failure count;
- physical total-size mismatch count versus `srv_log_file_size`;
- internal/physical size divergence count versus `log_sys.file_size`;
- observed physical total size accumulated over successful samples;
- observed physical allocated size accumulated over successful samples.

Expose the new counters from `mylite_embedded_performance_probe` detailed
startup output. Average physical size values use successful physical samples as
the denominator, not actual rebuild calls, so stat failures remain visible.

This slice does not change the redo rebuild decision, no-rebuild cleanup path,
checkpointing, file creation, rename, encryption, or recovery order.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
behavior changes. This slice only adds internal diagnostics and probe output.

## Directory And Lifecycle Impact

No directory layout or file lifecycle change. The slice samples
`ib_logfile0` in the native InnoDB log directory inside the MyLite database
directory.

## Native Storage Impact

No native redo format, size, checkpoint, truncation, or recovery behavior
change. The counters observe physical redo size at the existing rebuild
decision.

## Build And Size Impact

The slice adds a small number of internal counter slots and one `stat(2)` call
only while startup attribution is enabled and the existing rebuild predicate is
about to rebuild redo. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced production performance probe and confirm new physical-size keys
  are emitted.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output distinguishes internal `log_sys.file_size` mismatch from
  physical `ib_logfile0` total-size mismatch at actual rebuild decisions.
- Probe output records whether physical total size equals configured
  `srv_log_file_size`, matches the internal `log_sys.file_size`, or cannot be
  sampled.
- Existing log-rebuild reason and timing keys remain available.
- Focused production embedded lifecycle tests pass.
- Docs record whether the next performance slice should optimize startup
  decision normalization, shutdown/truncation, or leave native redo
  reconciliation untouched.

## Verification Results

A reduced twenty-iteration production probe after adding physical-size counters
reported:

- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_calls=4`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_size_mismatch_calls=4`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_observed_file_size_bytes_avg=100663304.000`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_desired_file_size_bytes_avg=100663296.000`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_physical_size_sample_calls=4`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_physical_size_mismatch_calls=4`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_internal_physical_size_mismatch_calls=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_observed_physical_file_size_bytes_avg=100663304.000`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_total_ms_avg=162.922`.

The measured rebuild trigger is a real physical `ib_logfile0` size mismatch:
the physical file and `log_sys.file_size` both report `100663304` bytes, while
the configured `srv_log_file_size` is `100663296` bytes. The next performance
slice should therefore focus on why clean embedded shutdown leaves a physical
8-byte redo tail or whether MariaDB's shutdown checkpoint policy requires that
tail. A startup-only comparison skip would be unsafe without proving redo wrap
geometry and checkpoint validation remain correct.

## Risks And Follow-Up

Physical stat evidence can still vary by filesystem and run timing. Any later
behavior change must keep MariaDB native redo recovery safe and must be covered
by focused open/close and crash/recovery tests. If the physical size is already
configured correctly, the next optimization should still prove that
normalizing `log_sys.file_size` preserves redo wrap boundaries and checkpoint
validation before skipping a rebuild.
