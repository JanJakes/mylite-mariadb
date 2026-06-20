# Embedded InnoDB Redo Clean-Shutdown Tail Truncation

## Problem

Physical-size attribution showed that some clean embedded open/close workloads
leave `ib_logfile0` physically 8 bytes larger than the configured
`srv_log_file_size`. The next startup attaches the physical file size into
`log_sys.file_size`, `srv_log_rebuild_if_needed()` sees a size mismatch, and
MariaDB performs a full redo rebuild even though the redo format already
matches.

A reduced twenty-open production probe reported four rebuilds averaging
`162.922 ms` each, with `log_sys.file_size` and physical `ib_logfile0` both at
`100663304` bytes versus the configured `100663296` bytes. The public
open/close bench can finish at the configured size, so the tail is
workload/LSN-position dependent rather than every close.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0log.cc:logs_empty_and_mark_files_at_shutdown()`
  waits for active work to stop, flushes the buffer pool, calls
  `log_make_checkpoint()`, and only reaches `SRV_SHUTDOWN_LAST_PHASE` after
  `lsn == log_get_lsn()` and the clean checkpoint/LSN checks pass.
- The same shutdown path accepts a final LSN equal to
  `last_checkpoint_lsn + SIZE_OF_FILE_CHECKPOINT` for non-encrypted redo, or
  `last_checkpoint_lsn + SIZE_OF_FILE_CHECKPOINT + 8` for encrypted redo.
- `mariadb/storage/innobase/buf/buf0flu.cc:log_checkpoint_low()` writes the
  checkpoint header after `fil_names_clear(oldest_lsn)` and `log_write_up_to()`
  have flushed redo through the FILE_CHECKPOINT boundary.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit_files()` emits the
  FILE_CHECKPOINT mini-transaction and appends a 5-byte non-encrypted trailer
  or a 13-byte encrypted trailer.
- `mariadb/storage/innobase/srv/srv0start.cc:create_log_file()` creates a
  replacement redo file at exactly `srv_log_file_size` with
  `os_file_set_size()`.
- `mariadb/storage/innobase/os/os0file.cc:os_file_set_size()` uses `fallocate`
  or `ftruncate` to the exact requested size on Linux; file creation is not
  expected to request `srv_log_file_size + 8`.
- `mariadb/storage/innobase/include/log0log.h:log_file_t` keeps the native file
  handle private. A narrow `log_t` helper can truncate the already-open redo
  file without exposing the handle outside the log subsystem.

## Design

Add an embedded-only clean-shutdown tail normalization step after
`logs_empty_and_mark_files_at_shutdown()` proves the shutdown LSN and checkpoint
state, before the redo log subsystem is closed:

- sample physical `ib_logfile0` size by filename;
- only continue when `log_sys.file_size == srv_log_file_size`;
- only continue when physical size is greater than `log_sys.file_size`;
- truncate the open redo file back to `log_sys.file_size`;
- do nothing for read-only, force-recovery, fastest shutdown, failed stat, or
  any case where the physical size is already equal to or smaller than the
  logical size.

The fix does not alter `srv_log_rebuild_if_needed()`, redo format checks,
checkpoint-header writes, FILE_CHECKPOINT records, redo encryption, crash
recovery, or log-file creation. It only removes bytes beyond the logical redo
file size after a clean embedded shutdown has already completed its native
checks.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, or WordPress behavior changes.
The intended effect is lower repeated process startup cost by avoiding
unnecessary redo rebuilds caused by physical bytes outside MariaDB's configured
redo file size.

## Directory And Lifecycle Impact

The change applies only to `ib_logfile0` inside the MyLite database directory
and only during clean embedded shutdown. It does not create new files or move
durable state outside the database directory.

## Native Storage Impact

The native redo file remains in MariaDB format and at MariaDB's configured
`innodb_log_file_size`. Bytes beyond `log_sys.file_size` are outside the
logical redo capacity that MariaDB should attach on the next startup. Focused
open/close and recovery tests must prove that truncation does not break clean
reopen.

## Build And Size Impact

The change adds one internal helper and one embedded-only shutdown stat/truncate
branch. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe`,
  `mylite_embedded_open_close_test`, and `mylite_public_open_close_bench` with
  `php-embedded-prod`.
- Run a reduced twenty-open production probe and confirm ordinary warm
  open/close no longer reports physical-size redo rebuilds in that sample.
- Run the public open/close bench and verify `ib_logfile0` remains at
  `100663296` bytes after close.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`,
  `git diff --check`, and `git diff --cached --check`.

## Acceptance Criteria

- Repeated clean embedded open/close does not leave `ib_logfile0` physically
  larger than `srv_log_file_size` in the focused repro.
- The reduced production probe reports zero actual redo rebuilds in the
  ordinary warm-open sample when the only previous trigger was the physical
  8-byte tail.
- Focused lifecycle tests and production build/format checks pass.
- Docs record the optimization scope and the remaining risk that broader crash
  or encrypted-redo coverage must precede any more aggressive redo policy
  changes.

## Verification Results

A reduced twenty-iteration production probe after adding clean-shutdown tail
truncation reported:

- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_calls=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_size_mismatch_calls=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_physical_size_sample_calls=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_physical_size_mismatch_calls=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_internal_physical_size_mismatch_calls=0`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_total_ms_avg=0.000`;
- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=124.153`.

The same production build's public open/close bench over twenty iterations
reported `mylite_public_open_close_avg_ms=109.834`, and `stat(2)` showed
`ib_logfile0` at `100663296` bytes after close. Focused production lifecycle
coverage passed with `libmylite.embedded-open-close`.

## Risks And Follow-Up

This slice deliberately avoids changing unclean shutdown or crash recovery. If
future evidence shows the tail can carry required encrypted-redo or crash
recovery state, the truncation guard must be narrowed further or removed. A
later slice can add explicit crash/recovery and encrypted-redo matrix coverage
before broadening the optimization.
