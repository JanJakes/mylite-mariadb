# No-Binlog Core Size Profile

## Problem

The `binlog-replication-size-profile` slice removed command-level binlog replay
and replication sources, but the linked MyLite runtime still retains MariaDB's
binary-log core. The remaining size root is not `BINLOG` SQL execution; it is
ordinary SQL, transaction, row-event, GTID, and system-variable code that still
references `mysql_bin_log`, `binlog_tp`, `Log_event`, `Rows_log_event`,
`Query_log_event`, and `Gtid_index_writer`.

Current reference point:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 33,676,708 |
| stripped `mylite-open-close-smoke` | 6,750,400 |

The largest retained linked symbols still include `MYSQL_BIN_LOG::open()`,
`MYSQL_BIN_LOG::write()`, `MYSQL_BIN_LOG::write_transaction_to_binlog()`,
`MYSQL_BIN_LOG::trx_group_commit_with_engines()`, `Query_log_event`,
`Table_map_log_event`, `Rows_log_event`, `Gtid_log_event`,
`rpl_binlog_state`, and `Gtid_index_writer`.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/log.h` defines `MYSQL_LOG::is_open()` inline as
  `return log_state != LOG_CLOSED;`. Because this is not compile-time false,
  many `if (mysql_bin_log.is_open())` blocks still instantiate
  `Query_log_event`, `Rows_log_event`, and GTID helpers even though MyLite's
  embedded runtime never opens a binlog.
- `vendor/mariadb/server/sql/log.cc` defines global `MYSQL_BIN_LOG
  mysql_bin_log(&sync_binlog_period)` and `transaction_participant binlog_tp`.
  `binlog_init()` installs `binlog_tp` through the mandatory `binlog` daemon
  plugin.
- `vendor/mariadb/server/sql/sql_builtin.cc.in` hard-codes
  `builtin_maria_binlog_plugin` into `mysql_mandatory_plugins`. The plugin is
  therefore present even when MyLite disables user-facing replay and
  replication sources.
- `vendor/mariadb/server/sql/handler.cc` compares transaction participants
  against `&binlog_tp`, runs `binlog_commit()` or `binlog_rollback()` first
  through `run_binlog_first()`, and calls `binlog_commit_by_xid()` /
  `binlog_rollback_by_xid()` for recovered XA transactions.
- `vendor/mariadb/server/sql/handler.cc` also has row logging through
  `handler::binlog_log_row()` and `binlog_log_row_to_binlog()`, which calls
  `THD::binlog_write_table_maps()`, `THD::binlog_setup_trx_data()`, and
  row-event creators.
- `vendor/mariadb/server/sql/sql_class.cc` implements
  `THD::binlog_write_row()`, `THD::binlog_update_row()`,
  `THD::binlog_delete_row()`, `THD::binlog_flush_pending_rows_event()`, and
  `THD::binlog_query()`. These functions create row/query log events and keep
  `pack_row()` and row-event classes live.
- `vendor/mariadb/server/sql/log.cc` implements
  `THD::binlog_setup_trx_data()`, `THD::binlog_start_trans_and_stmt()`,
  `THD::binlog_write_table_maps()`, `binlog_commit()`,
  `binlog_rollback()`, GTID state helpers, and `MYSQL_BIN_LOG` methods. These
  roots keep `log_cache.cc`, `log_event*.cc`, `rpl_gtid.cc`, and
  `gtid_index.cc` live.
- The current minsize archive still contains the following related objects:

| Object | Bytes |
| --- | ---: |
| `rpl_utility_server.cc.o` | 388,336 |
| `log.cc.o` | 324,840 |
| `log_event_server.cc.o` | 276,856 |
| `log_event.cc.o` | 133,768 |
| `rpl_gtid.cc.o` | 123,368 |
| `gtid_index.cc.o` | 57,952 |
| `rpl_filter.cc.o` | 36,912 |
| `rpl_injector.cc.o` | 14,016 |
| `log_cache.cc.o` | 7,032 |
| `rpl_utility.cc.o` | 5,424 |
| `rpl_record.cc.o` | 1,936 |

`rpl_utility_server.cc.o` is intentionally not the first target: despite the
replication name, retained field conversion hooks use it outside command-level
replication code.

## Scope

This slice may add a MyLite-owned `MYLITE_DISABLE_BINLOG_CORE` aggressive
minsize option that:

- makes `mysql_bin_log.is_open()` and related user-facing checks compile-time
  false in the embedded minsize profile,
- excludes the `binlog` daemon plugin from mandatory built-ins when the profile
  is enabled,
- no-ops `binlog_commit()`, `binlog_rollback()`,
  `binlog_commit_by_xid()`, `binlog_rollback_by_xid()`,
  `THD::binlog_query()`, `THD::binlog_flush_pending_rows_event()`, and row
  write helpers,
- keeps a minimal `mysql_bin_log` object only where inherited code still needs
  an address, a lock object, or a harmless `TC_LOG` fallback,
- removes source objects only after the guarded build proves they are no longer
  referenced, and
- records exact archive and stripped-linked deltas for each successful or
  rejected source removal.

## Non-Goals

This slice does not:

- remove parser grammar for binlog, replication, or `SHOW BINLOG` statements,
- remove `rpl_utility_server.cc` unless source evidence proves retained type
  conversion hooks are gone,
- change MyLite public C API or file format,
- implement replacement replication or point-in-time recovery behavior, or
- change non-minsize MariaDB-derived builds.

## Proposed Design

Add `MYLITE_DISABLE_BINLOG_CORE` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it only from
`tools/build-mariadb-minsize.sh`.

First, introduce compile-time no-binlog gates while leaving sources in place:

- In `log.h`, make `MYSQL_BIN_LOG::is_open()` hide the inherited
  `MYSQL_LOG::is_open()` and return `false` under `MYLITE_DISABLE_BINLOG_CORE`
  for embedded builds.
- In `sql_builtin.cc.in`, omit `builtin_maria_binlog_plugin` from mandatory
  plugins under the same macro.
- In `handler.cc`, compile `run_binlog_first()` and recovered-XA binlog calls
  as no-ops, and avoid treating `&binlog_tp` as a participating storage engine.
- In `sql_class.cc`, compile `THD::binlog_query()`,
  `THD::binlog_flush_pending_rows_event()`, and row-event write helpers as
  no-ops.
- In `log.cc`, compile `binlog_commit()`, `binlog_rollback()`,
  `binlog_commit_by_xid()`, `binlog_rollback_by_xid()`,
  `THD::binlog_setup_trx_data()`, `THD::binlog_start_trans_and_stmt()`, and
  `THD::binlog_write_table_maps()` as no-ops or null-returning helpers.

Second, rebuild with all binlog-core sources still present. If smokes pass,
measure whether section GC drops the event/GTID code from the linked artifact.
This step validates the semantic guards before source-list pruning.

Third, try source-list removal in small batches:

1. `log_event_server.cc`, `rpl_gtid.cc`, `gtid_index.cc`, `rpl_filter.cc`,
   `rpl_injector.cc`, and `rpl_record.cc`
2. `log_event.cc`
3. `log_cache.cc`

Keep `log.cc` until a minimal replacement object is proven. It contains general
logging, the global `logger`, SQL print helpers, TC_LOG helpers, `mysql_bin_log`,
and `binlog_tp`, so removing it wholesale is higher risk than compiling the
binlog-specific entry points to no-ops.

## Affected Subsystems

- Embedded source list and mandatory plugin list.
- Transaction commit, rollback, savepoint, recovered-XA, and row-write hooks.
- Statement, DDL, trigger, and stored-program binlog helper calls.
- System variables and status variables that reference binlog/GTID state.
- Sidecar-file behavior for binlog, GTID index, relay log, and binlog cache
  temporary files.

## Single-File and Embedded-Lifecycle Impact

This slice aligns the aggressive profile with MyLite's embedded single-file
shape: no binary log files, relay logs, GTID index files, or binlog-cache files
should be opened or retained.

The implementation must preserve ordinary MyLite transaction lifecycle:
create, insert, select, rollback, commit, savepoint, and repeated open/close
must still work through MyLite's supported storage path.

## Public API and File-Format Impact

No public `libmylite` API change and no MyLite file-format change.

Replication, binlog replay, and binlog administration remain intentionally
unsupported in the aggressive embedded profile.

## Binary-Size Impact

The directly related archive objects add up to about 1.31 MiB, but
`rpl_utility_server.cc` is not a safe first target and `log.cc` cannot be
removed wholesale in this slice. A realistic successful first pass should aim
for hundreds of KiB of archive reduction and a measurable linked-runtime
reduction if `Query_log_event`, `Rows_log_event`, `Table_map_log_event`,
`Gtid_log_event`, and `Gtid_index_writer` drop from the linked smoke binary.

Rejected attempts are still useful if they identify hard source roots.

Implemented first-pass measurements:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 33,532,138 |
| stripped `mylite-open-close-smoke` | 6,683,128 |

Compared with `binlog-replication-size-profile`, the first pass saves 144,570
archive bytes and 67,272 stripped linked bytes. Most of that linked win comes
from compiling the binlog transaction, row-event, GTID-state, and event-write
entry points to no-ops so section GC can discard now-unreachable code.

The only source-list removal accepted in this pass is `rpl_record.cc`
(`rpl_record.cc.o`, 2,152 bytes in the current object build). Removing it has
no linked-runtime effect but confirms `pack_row()` is no longer rooted by row
binlog helpers.

The broader source-list removal attempt for `log_event.cc`,
`log_event_server.cc`, `rpl_gtid.cc`, `gtid_index.cc`, `rpl_filter.cc`, and
`rpl_injector.cc` built the archive but failed the final smoke executable link.
The unresolved roots were:

- `lib_sql.cc` startup and cleanup still call `injector::free_instance()`,
  `Gtid_index_writer::gtid_index_init()`,
  `Gtid_index_writer::gtid_index_cleanup()`, and allocate/delete
  `Rpl_filter`.
- `table.cc` still calls `binlog_filter->db_ok()` in table-open filtering.
- `log.cc` still retains `Log_event_writer`, `Format_description_log_event`,
  `Binlog_checkpoint_log_event`, `MYSQL_BIN_LOG::open()`,
  `MYSQL_BIN_LOG::do_binlog_recovery()`, and `rpl_binlog_state` roots.

Those roots are larger than the accepted first-pass cut, but they need a
separate guarded cleanup because they cross embedded startup, table-open, sysvar
and generic log-helper code rather than only row/statement binlog entry points.

## Test and Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-core MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-core MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-core MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
git diff --check
```

Measure:

- `libmysqld/libmariadbd.a` bytes and object count,
- stripped `mylite-open-close-smoke` bytes,
- `size` section totals,
- retained `MYSQL_BIN_LOG`, `Log_event`, row-event, GTID, and `rpl_*` symbols,
- dynamic dependencies, and
- sidecar scan results.

The smoke/harness coverage must continue to exercise:

- ordinary open/close,
- create/insert/select,
- commit/rollback/savepoint behavior,
- unsupported `BINLOG` diagnostics, and
- no unexpected MariaDB sidecars.

## Acceptance Criteria

- The aggressive minsize build passes current MyLite smokes and compatibility
  harness.
- `BINLOG` and replication surfaces remain explicitly unsupported.
- No binlog, relay-log, GTID-index, or binlog-cache sidecar files appear.
- Size deltas are recorded in `docs/research/production-size-analysis.md`.
- Any source-list removal is backed by successful link and smoke evidence.
- The implementation remains guarded by MyLite minsize macros and does not
  change default MariaDB-derived builds.

## Risks and Unresolved Questions

- Making `MYSQL_LOG::is_open()` compile-time false is broad. It may also affect
  relay-log or generic log code if the guard is not scoped tightly to the
  MyLite embedded profile.
- `log.cc` also owns general logging and `TC_LOG` helpers, so replacing it with
  a minimal stub may be a later slice.
- Some binlog state is exposed through system variables even when binlogging is
  unavailable. These may need explicit disabled-variable behavior rather than
  accidental zero values.
- Recovered XA paths may depend on binlog hooks for server correctness. MyLite
  must verify ordinary XA diagnostics and transaction cleanup before accepting
  this profile.
- `rpl_utility_server.cc` may remain because field/type conversion hooks are
  retained outside replication command execution.
