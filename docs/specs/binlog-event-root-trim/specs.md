# Binlog Event Root Trim

## Problem

The default embedded profile now disables binlog transaction and event-write
entry points, omits `rpl_record.cc`, and rejects replication/binlog SQL at the
public API boundary. The embedded archive still carries `gtid_index.cc`,
`log_event.cc`, and `rpl_injector.cc`, plus binlog open/recovery, GTID-index,
and annotated-row helper roots that are unreachable when `--skip-log-bin` and
`MYLITE_WITH_BINLOG_CORE=0` are both active.

Those roots preserve server topology and binlog-file maintenance code inside
the file-owned embedded profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/CMakeLists.txt` still includes `../sql/gtid_index.cc`,
  `../sql/log_event.cc`, `../sql/log_event_server.cc`, `../sql/rpl_gtid.cc`,
  and `../sql/rpl_injector.cc` in `SQL_EMBEDDED_SOURCES`.
- `mariadb/sql/log.cc` includes `gtid_index.h`, owns
  `Gtid_index_writer *gtid_index`, starts binlog recovery and the binlog
  background thread in `MYSQL_BIN_LOG::open()`, writes format-description
  events through `Event_log::write_description_event()`, reports
  `MYSQL_BIN_LOG::get_binlog_space_total()`, writes annotated row events in
  `THD::binlog_write_annotated_row()`, and updates/closes GTID indexes through
  `MYSQL_BIN_LOG::update_gtid_index()` and background jobs.
- `mariadb/sql/mysqld.cc` includes `gtid_index.h` and `rpl_injector.h`,
  initializes and cleans up `Gtid_index_writer`, and frees the replication
  injector singleton during shutdown.
- `mariadb/sql/log.h` chooses `mysql_bin_log` as the transaction coordinator
  implementation when `opt_bin_log` is true and more than two two-phase
  participants exist.
- `nm -u build/mariadb-embedded/libmysqld/libsql_embedded.a` after the
  first no-binlog-core slice still shows external references to
  `Gtid_index_writer`, `Gtid_index_base::make_gtid_index_file_name()`,
  `injector::free_instance()`, `str_to_hex()`, `append_query_string()`,
  `Log_event_writer`, `Rows_log_event`, `Gtid_log_event`, and
  `Table_map_log_event`.
- Historical branch commit `7521fe3ff3a` removed `gtid_index.cc`,
  `log_event.cc`, and `rpl_injector.cc` by guarding no-binlog startup,
  cleanup, open/recovery, status, and annotated-row paths and adding a small
  `mylite_log_event_core_stub.cc`. The paths and option names changed in the
  current tree, so this must be reapplied from source evidence.

## Design

- Keep using `MYLITE_WITH_BINLOG_CORE=0` as the controlling embedded-profile
  option. Do not add an independent flag unless the implementation proves the
  boundary is larger than this slice.
- Remove `../sql/gtid_index.cc`, `../sql/log_event.cc`,
  `../sql/rpl_injector.cc`, and `../sql/rpl_record.cc` from
  `SQL_EMBEDDED_SOURCES` only when
  `MYLITE_WITH_BINLOG_CORE=OFF`.
- Add a MyLite-owned embedded stub in `mariadb/libmysqld/` for retained
  no-binlog symbols that are still referenced by shared MariaDB code.
- Guard current-source roots that should be unreachable in the disabled
  embedded profile:
  - `MYSQL_BIN_LOG::open()` should return success without opening binlog files,
    doing GTID recovery, or starting the binlog background thread.
  - `MYSQL_BIN_LOG::cleanup()` should destroy initialized synchronization
    objects without deleting log-event state that is never allocated.
  - `MYSQL_BIN_LOG::get_binlog_space_total()` should report `0`.
  - Binlog transaction, row-event, table-map, GTID-event, event-write,
    cache-write, incident, checkpoint, and temporary-table cleanup event paths
    should return no-op success or false error status without constructing
    event objects.
  - `THD::binlog_write_annotated_row()` should return no error without
    constructing `Annotate_rows_log_event`.
  - `mysqld.cc` should skip `Gtid_index_writer` init/cleanup and
    `injector::free_instance()` in the disabled embedded profile.
  - `get_tc_log_implementation()` should avoid choosing `mysql_bin_log` as
    the transaction coordinator in the disabled embedded profile.
- Keep `log_event_server.cc` and `rpl_gtid.cc` in scope only if they are still
  needed by retained shared code. Removing them is a follow-up unless a small
  stub boundary is obvious and verified in this slice.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- Binary-log file open/recovery, GTID-index, background-job, and annotated-row
  helper roots.
- Embedded startup/shutdown cleanup order.
- Server-surface, size-profile, and build-measurement documentation.

## MySQL/MariaDB Compatibility Impact

Replication and binary logging remain out of scope for MyLite's embedded core.
Supported MyLite SQL behavior must keep working with binlog file maintenance
compiled out. The normal MariaDB `sql` target must retain the full binlog,
event, injector, and GTID-index implementation for comparison builds.

## Single-File And Embedded-Lifecycle Impact

The disabled profile must not create binlog, relay-log, GTID-index, binlog
index, binlog-state, or binlog-cache sidecars. Startup and shutdown must remain
repeatable across the existing embedded lifecycle tests.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Public replication/binlog SQL
rejection remains a server-surface policy behavior.

## Storage-Engine Routing Impact

No direct storage-engine routing change. Routed table DDL/DML and statement
rollback must not depend on binlog file maintenance, GTID indexes, or
replication injector state.

## Binary-Size Impact

Historical branch-level research measured this cut as 58,880 bytes saved in
the stripped linked open-close smoke and 240,220 bytes saved in the embedded
archive. The current baseline has a different trim stack, so this slice records
fresh default and storage-smoke archive measurements before acceptance.

Measured on 2026-05-15 after implementation:

| Profile | Archive size | Archive members | Change from previous no-binlog core baseline |
| --- | ---: | ---: | ---: |
| Default embedded | 27,607,216 bytes / 26.33 MiB | 679 | -146,128 bytes / -2 members |
| Storage-smoke | 27,787,792 bytes / 26.50 MiB | 682 | -146,128 bytes / -2 members |

At the time of this slice, the disabled embedded archives contained
`mylite_log_event_core_stub.cc.o` and `log_event_server.cc.o`, but omitted
`gtid_index.cc.o`, `log_event.cc.o`, `rpl_injector.cc.o`, and
`rpl_record.cc.o`. The follow-up log-event server trim now replaces
`log_event_server.cc.o` with `mylite_log_event_server_disabled.cc.o` while
keeping the ordinary SQL string-rendering root in
`mylite_log_event_core_stub.cc.o`.

## License And Dependency Impact

No new dependency. The change adds a small GPL-compatible first-party stub and
removes MariaDB-derived source objects from the disabled embedded profile only.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both embedded archives omit any removed source objects.
- Confirm the normal MariaDB `sql` target still builds the removed sources.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Implementation Notes

- `mariadb/libmysqld/mylite_log_event_core_stub.cc` provides the retained
  no-binlog symbols still needed by shared MariaDB code:
  `key_memory_Incident_log_event_message`, `binlog_checksum_typelib`,
  `str_to_hex()`, and `append_query_string()`.
- `mariadb/sql/log.cc` guards disabled-profile binlog transaction/event roots,
  binlog open/recovery, GTID-index lifecycle, event/cache writes, incidents,
  checkpoints, and background-thread/recovery helpers.
- `mariadb/sql/log.h` keeps the disabled embedded profile on `tc_log_mmap`
  instead of choosing `mysql_bin_log` when `opt_bin_log` is true.
- `mariadb/sql/mysqld.cc` skips GTID-index initialization/cleanup and
  replication-injector cleanup in the disabled embedded profile.
- `mariadb/sql/temporary_tables.cc` keeps temporary-table cleanup but skips the
  no-binlog cleanup-event write that would otherwise construct
  `Query_log_event`.
- `log_event_server.cc` and `rpl_gtid.cc` remained in the archive after this
  slice. The follow-up log-event server trim removes `log_event_server.cc` by
  splitting retained SQL string rendering from disabled event-class symbols.
  `rpl_gtid.cc` still requires a separate GTID-state stub boundary.

## Verification Results

Passed on 2026-05-15:

- `tools/mariadb-embedded-build configure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `cmake --build --preset embedded-dev`
- `cmake --build --preset storage-smoke-dev`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `tools/mylite-size-report`
- `cmake --build build/mariadb-embedded --target sql`
- `cmake --build --preset dev`
- `ctest --preset dev --output-on-failure`
- `cmake --build --preset dev --target format-check`
- `bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report`
- `git diff --check`
- `cmake --build --preset dev --target tidy`

## Acceptance Criteria

- The default embedded and storage-smoke archives have measured size reductions.
- The disabled embedded archives omit `gtid_index.cc.o`, `log_event.cc.o`,
  `rpl_injector.cc.o`, and `rpl_record.cc.o`.
- Supported MyLite open/close, direct SQL, prepared SQL, warnings, comparison,
  storage-smoke, and server-surface coverage still pass.
- No binlog, relay-log, GTID-index, binlog-index, binlog-state, or
  binlog-cache sidecar is introduced.
- The diff is guarded to the MyLite embedded profile and preserves the normal
  MariaDB SQL target.
- Documentation records the exact boundary, including retained event or GTID
  objects that could not be removed safely in this slice.

## Risks And Unresolved Questions

- This slice left `log_event_server.cc` as follow-up work because it provided
  event serialization helpers and shared SQL string-rendering code used by
  retained paths. The later log-event server trim handles that boundary.
- `rpl_gtid.cc` owns global GTID state used by retained system variables and
  no-binlog stubs. Removing it is intentionally out of scope for this slice.
- `MYSQL_BIN_LOG::cleanup()` is shutdown-sensitive; the disabled path must only
  skip cleanup for objects that the disabled startup/open paths never allocate.
