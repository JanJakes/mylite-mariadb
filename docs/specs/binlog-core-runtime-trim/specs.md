# Binlog Core Runtime Trim

## Problem

MyLite starts embedded MariaDB with `--skip-log-bin`, rejects `BINLOG` and
replication SQL at the public API boundary, and treats binary logs, relay logs,
and GTID index files as server-side sidecars outside the single-file product
shape. The default embedded archive still links MariaDB's binary-log transaction
participant, row-event writing paths, GTID state helpers, and mandatory binlog
plugin registration.

Those paths are unreachable for supported MyLite behavior, but they keep
server topology code and event-format code in the embedded profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc` starts embedded MariaDB with
  `--skip-log-bin` and rejects `BINLOG`, replication commands, and replication
  wait helpers before MariaDB execution.
- `mariadb/libmysqld/CMakeLists.txt` still includes `../sql/log.cc`,
  `../sql/log_cache.cc`, `../sql/log_event.cc`,
  `../sql/log_event_server.cc`, `../sql/rpl_record.cc`,
  `../sql/rpl_injector.cc`, `../sql/rpl_gtid.cc`, and
  `../sql/gtid_index.cc` in `SQL_EMBEDDED_SOURCES`.
- `mariadb/sql/sql_builtin.cc.in` registers `builtin_maria_binlog_plugin` as a
  mandatory plugin.
- `mariadb/sql/handler.cc` special-cases `binlog_tp` in transaction commit and
  rollback ordering and writes row events through `handler::binlog_log_row()`.
- `mariadb/sql/sql_class.cc` constructs row and query binlog events through
  `THD::binlog_write_row()`, `THD::binlog_update_row()`,
  `THD::binlog_delete_row()`, `THD::binlog_flush_pending_rows_event()`, and
  `THD::binlog_query()`.
- `mariadb/sql/log.cc` owns `mysql_bin_log`, `binlog_tp`, binlog cache setup,
  transaction commit/rollback hooks, GTID state accessors, table-map writes,
  and `TC_LOG_BINLOG` methods.

## Design

- Add `MYLITE_WITH_BINLOG_CORE`, defaulting to `ON` for upstream-compatible
  embedded builds.
- Set `MYLITE_WITH_BINLOG_CORE=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When disabled in `mariadb/libmysqld/CMakeLists.txt`, define
  `MYLITE_WITH_BINLOG_CORE=0` and remove source objects only after guarded code
  proves they are not referenced.
- Keep `log.cc` in the embedded source list. It owns generic logging,
  `mysql_bin_log`, transaction-coordinator symbols, and cleanup paths that are
  still shared with retained MariaDB code.
- Omit mandatory binlog plugin registration from `sql_builtin.cc.in` in the
  disabled embedded profile.
- Make embedded binlog transaction, row-event, GTID-state, event-write, and
  table-map entry points no-op or fail-closed behind
  `MYLITE_WITH_BINLOG_CORE=0`.
- Remove `../sql/rpl_record.cc` if the guarded row-event paths prove it is
  unreferenced. Broader removal of `log_event*.cc`, `rpl_gtid.cc`,
  `gtid_index.cc`, or `log_cache.cc` is a follow-up only if link evidence shows
  a narrow safe boundary.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- Mandatory plugin registration for the embedded profile.
- Transaction commit/rollback binlog participant hooks.
- Row-event and query-event binlog helper methods.
- GTID state helpers exposed through `MYSQL_BIN_LOG`.
- Server-surface policy tests and documentation.

## MySQL/MariaDB Compatibility Impact

Replication and binary logging remain out of scope for MyLite's embedded core.
Supported table DDL, DML, metadata, diagnostics, warnings, storage routing, and
statement rollback must keep working with binlogging compiled to no-ops.

The normal MariaDB `sql` target keeps the full binlog core for comparison
builds.

## Single-File And Embedded-Lifecycle Impact

The disabled profile must not create binlog, relay-log, GTID-index, or
binlog-cache sidecars. Existing sidecar gates remain the compatibility evidence
for routed storage smoke coverage.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Existing public rejection for
binlog and replication SQL remains `MYLITE_ERROR`, SQLSTATE `HY000`, MariaDB
errno `0`, and a stable MyLite diagnostic where MyLite policy catches the
statement.

## Storage-Engine Routing Impact

Supported MyLite-routed table behavior must not rely on binary logging.
`ENGINE=BLACKHOLE` and `ENGINE=MEMORY` / `ENGINE=HEAP` continue to exclude
native replication/binlog semantics from their compatibility target.

## Binary-Size Impact

Historical branch-level research measured the first no-binlog-core cut as
66,312 bytes saved in the stripped linked open-close smoke and 144,570 bytes
saved in the embedded archive. That measurement predates the current direct
session and size-trim stack, so this slice must rerun the candidate against the
current baseline before accepting it.

Current measured impact from the dynamic UDF runtime baseline:

| Profile | Archive size | Members | Delta from UDF baseline |
| --- | ---: | ---: | ---: |
| Default embedded | 27,753,344 bytes / 26.47 MiB | 681 | -76,880 bytes / -1 member |
| Storage-smoke | 27,933,920 bytes / 26.64 MiB | 684 | -76,880 bytes / -1 member |

The linked open-close smoke dropped from 17,780,512 bytes to 17,744,048
bytes, and its stripped size dropped from 16,087,712 bytes to 16,052,560
bytes. This was a narrower win than the historical branch result because the
baseline at the time still kept `log_event.cc`, `log_event_server.cc`,
`rpl_gtid.cc`, and `gtid_index.cc` in the archive; retained MariaDB code still
referenced those objects. Follow-up binlog event-root and log-event server
trims now remove `gtid_index.cc`, `log_event.cc`, `log_event_server.cc`,
`rpl_injector.cc`, and `rpl_record.cc` from the disabled profile.

## License And Dependency Impact

No new dependency. The change only gates MariaDB-derived runtime code behind a
MyLite embedded profile option.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm the default embedded cache records `MYLITE_WITH_BINLOG_CORE=OFF`.
- Confirm any removed source object is absent from both embedded archives.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Build the normal MariaDB `sql` target to confirm the non-embedded server path
  still compiles with full binlog code.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- Supported MyLite open/close, direct SQL, prepared SQL, warnings, comparison,
  storage-smoke, and server-surface coverage still pass.
- `BINLOG` and replication surfaces remain explicitly unsupported.
- No binlog, relay-log, GTID-index, or binlog-cache sidecar is introduced.
- The default embedded archive has a measured size reduction.
- Documentation and compatibility matrix describe the no-binlog embedded core.
- The diff is guarded to the MyLite embedded profile and preserves the normal
  MariaDB SQL target.

## Implementation Results

- `MYLITE_WITH_BINLOG_CORE=OFF` is set only by the MyLite embedded baseline.
  The option defaults to `ON`, and the normal MariaDB `sql` target still builds
  `rpl_record.cc`, `sql_udf.cc`, replication, semisync, and the full binlog
  code paths.
- The embedded archive omits `rpl_record.cc.o` in the default and
  storage-smoke profiles.
- `sql_builtin.cc.in` skips the mandatory binlog plugin entry only when the
  disabled embedded profile defines `MYLITE_WITH_BINLOG_CORE=0`.
- `handler.cc`, `log.cc`, `log.h`, and `sql_class.cc` keep their upstream code
  shape for normal builds and return no-op or fail-closed values for embedded
  binlog transaction, row-event, GTID-state, event-write, and table-map paths.
- Symbol probes still find `Rows_log_event`, `Gtid_log_event`, `binlog_commit`,
  and `builtin_maria_binlog_plugin` in linked smoke binaries because retained
  event and GTID objects remain shared with other MariaDB code. This slice does
  not claim full event-object removal.

Verified with:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `cmake --build --preset storage-smoke-dev`
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

## Risks And Unresolved Questions

- `log.cc` owns generic logging and transaction-coordinator symbols, so
  replacing it wholesale is not part of this slice.
- Recovered XA and row-event helper paths are server-correctness sensitive.
  They must be no-op only in the MyLite embedded profile where SQL transaction
  control and binlog surfaces are unsupported.
- Some GTID/binlog state may remain through system-variable plumbing. Follow-up
  source pruning must use link and smoke evidence, not file names alone.
