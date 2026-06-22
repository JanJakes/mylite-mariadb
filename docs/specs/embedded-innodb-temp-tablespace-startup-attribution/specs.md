# Embedded InnoDB Temporary Tablespace Startup Attribution

## Problem

After clean-shutdown redo-tail truncation and recovery-start attribution, the
remaining non-rebuild warm-open InnoDB startup cost was led by clean redo
scanning and temporary tablespace opening. The existing
`innodb_system_tables_open_tmp` bucket identified
`srv_open_tmp_tablespace()` as a stable cost, but it did not distinguish
leftover cleanup, file-spec validation, file create/open, fil-system open,
header initialization, or temporary rollback-segment creation.

MyLite needs that split before deciding whether any temporary-tablespace
lifecycle optimization is safe for ordinary embedded opens or ownerless
cross-process opens.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:srv_start()` calls
  `srv_open_tmp_tablespace(create_new_db)` while starting InnoDB system tables,
  after `dict_sys.create_or_check_sys_tables()` and before the master timer.
- `mariadb/storage/innobase/srv/srv0start.cc:srv_open_tmp_tablespace()` sets
  sanity-check state, calls `srv_tmp_space.delete_files()`, validates the
  temporary tablespace spec with a 12 MiB minimum, opens or creates the file,
  reopens it through `fil_system.temp_space`, initializes the header with
  `MTR_LOG_NO_REDO`, then creates the temporary rollback segment with
  `trx_temp_rseg_create()`.
- `mariadb/storage/innobase/fsp/fsp0sysspace.cc:SysTablespace::open_or_create()`
  opens an existing file or creates a missing file, closes those handles, adds
  a `fil_space_t`, and registers file nodes so the fil-system cache can hold
  them until shutdown.
- `mariadb/storage/innobase/handler/ha_innodb.cc` parses
  `innodb_temp_data_file_path` as a read-only startup option with the upstream
  default `ibtmp1:12M:autoextend`.
- `packages/libmylite/src/database.cc` passes the upstream temp tablespace name
  for ordinary embedded opens and a runtime-private `tmp/<runtime>/ibtmp1`
  path for ownerless read/write opens.

## Design

Extend the disabled-by-default startup counter table with a nested
temporary-tablespace group under the existing InnoDB system-table startup
bucket:

- total `srv_open_tmp_tablespace()` calls and elapsed time;
- leftover `delete_files()` time;
- `check_file_spec()` time;
- create-new versus reuse-existing count after successful spec validation;
- `open_or_create()` time;
- `fil_system.temp_space->open(true)` time;
- `fsp_header_init()` time;
- `trx_temp_rseg_create()` time.

Expose the dominant children in compact `mylite_perf_summary_*` output so CI
can show whether the next startup target is filesystem create/unlink,
fil-system open, header initialization, or temp rollback-segment creation.
Expose all counters in detailed probe output for local profiling.

This slice observes the existing MariaDB startup path only. It does not skip
cleanup, change the 12 MiB minimum, move ordinary temp tablespace files, change
ownerless private temp tablespace routing, or change shutdown cleanup.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
behavior changes. The slice only adds internal performance counters and probe
output.

## Directory And Lifecycle Impact

No database-directory layout change. Ordinary embedded opens continue to use
the upstream `ibtmp1:12M:autoextend` setting, and ownerless read/write opens
continue to use a runtime-private `tmp/<runtime>/ibtmp1` path.

## Native Storage Impact

No native InnoDB format, temporary tablespace, redo, undo, or recovery
semantics change. The new counters wrap existing calls without changing their
order or predicates.

## Build And Size Impact

The slice adds enum slots and timestamp reads when startup attribution is
enabled. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` and
  `mylite_embedded_open_close_test` with the production embedded PHP preset.
- Run a reduced production performance probe and confirm the
  `innodb_temp_tablespace_*` keys are emitted in compact and detailed output.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output splits `srv_open_tmp_tablespace()` into cleanup, validation,
  file open/create, fil open, header initialization, and temp rollback-segment
  creation.
- Compact summaries expose enough temporary-tablespace timing to explain the
  existing `innodb_system_tables_open_tmp` bucket.
- Focused production embedded lifecycle tests pass.
- Documentation records the measured next startup optimization target without
  claiming a behavior change.

## Verification Results

A reduced five-iteration production probe after adding the counters reported
the ordinary warm-open temporary-tablespace breakdown:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=136.999`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_total_ms_avg=43.733`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_ms_avg=20.240`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_scan_initial_ms_avg=16.145`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_scan_rescan_ms_avg=4.085`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_system_tables_open_tmp_ms_avg=12.255`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_total_ms_avg=12.255`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_delete_files_ms_avg=0.016`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_check_file_spec_ms_avg=0.010`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_temp_tablespace_create_new_calls=5`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_temp_tablespace_reuse_existing_calls=0`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_open_or_create_ms_avg=10.007`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_fil_open_ms_avg=0.029`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_header_init_ms_avg=0.036`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_temp_tablespace_rseg_create_ms_avg=2.153`.

The ownerless warm-open sample reported the same shape:

- `mylite_perf_summary_ownerless_warm_open_close_ms_avg=139.180`;
- `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_srv_start_total_ms_avg=62.227`;
- `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_recovery_start_ms_avg=19.603`;
- `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_system_tables_open_tmp_ms_avg=10.853`;
- `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_temp_tablespace_total_ms_avg=10.853`;
- `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_temp_tablespace_create_new_calls=5`;
- `mylite_perf_ownerless_warm_open_close_startup_phase_innodb_temp_tablespace_reuse_existing_calls=0`;
- `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_temp_tablespace_open_or_create_ms_avg=8.608`;
- `mylite_perf_summary_ownerless_warm_open_close_startup_innodb_temp_tablespace_rseg_create_ms_avg=2.140`.

The remaining temporary-tablespace startup cost is therefore dominated by
repeated file create/open work for a newly created 12 MiB temporary tablespace,
with temporary rollback-segment creation as the second visible child. Cleanup,
file-spec validation, fil-system open, and header initialization were
sub-millisecond in this sample.

The same ownerless warm sample still included actual redo rebuild time on some
iterations (`startup_innodb_srv_start_log_rebuild_ms_avg=23.647`,
`startup_innodb_log_rebuild_total_ms_avg=116.325`), so redo rebuild trigger
frequency remains a separate high-impact follow-up from temporary-tablespace
startup attribution.

## Risks And Follow-Up

The follow-up `../embedded-innodb-temp-tablespace-sparse-size/specs.md` slice
addresses the measured file create/open child by sizing newly created temporary
tablespace files sparsely while keeping InnoDB temp header initialization and
temporary rollback-segment creation intact. The follow-up
`../embedded-innodb-temp-rseg-startup-attribution/specs.md` slice then splits
the remaining temp rollback-segment bucket before any native transaction
assignment change. Further startup optimization should target clean redo scan or
temp rollback-segment creation with separate clean shutdown, crash recovery,
read-only startup, ordinary embedded open, ownerless cross-process open, and
runtime cleanup evidence.
