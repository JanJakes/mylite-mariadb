# Binlog Injector Root Trim

## Problem Statement

The embedded profile already rejects replication and binlog SQL, starts with
`--skip-log-bin`, reports `@@log_bin=0`, and omits the active binlog
transaction/event core. After that first trim, the archive still retained
no-binlog startup, open, cleanup, annotated-row, and GTID-index update paths
from `log.cc` and process-global injector initialization from `mysqld.cc`.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/libmysqld/CMakeLists.txt` still linked `rpl_injector.cc` into
  `sql_embedded` when `MYLITE_WITH_BINLOG_CORE=OFF`.
- `mariadb/sql/log.cc` still opened binlogs, cleaned up global GTID state,
  updated GTID indexes, computed binlog disk use, and could build annotated row
  events through retained methods even though the supported embedded startup
  path keeps binlog disabled.
- `mariadb/sql/mysqld.cc` still initialized and cleaned GTID-index and injector
  process-global state in the embedded no-binlog profile.
- `mariadb/sql/log.h` still allowed `get_tc_log_implementation()` to select the
  binlog transaction coordinator if inherited option state drifted.
- `packages/libmylite/src/database.cc` already passes `--skip-log-bin` and
  rejects binlog and replication command families before dispatch.
- `packages/libmylite/tests/embedded_server_surface_policy_test.c` already
  covers `@@log_bin=0` and absence of binlog or relay-log sidecars.

An attempted broader event-object trim showed that `gtid_index.cc`,
`log_event.cc`, and `log_event_server.cc` are still linked through retained
MariaDB code. This slice keeps them instead of replacing shared event and
GTID-index implementations with a wide stub.

## Design

When `MYLITE_WITH_BINLOG_CORE=OFF`:

- remove `rpl_injector.cc` and the already-omitted `rpl_record.cc` from
  `sql_embedded`;
- make `MYSQL_BIN_LOG::open()` a no-op success path;
- make binlog disk-use, annotated-row, and GTID-index update paths inert;
- skip GTID-index and injector startup/cleanup;
- force the embedded transaction-coordinator selection away from
  `mysql_bin_log` when the binlog core is disabled.

Normal MariaDB server builds keep the upstream binlog event, GTID-index, and
injector sources.

## Compatibility Impact

No application SQL compatibility impact is expected. Replication, binlog, relay
log, and binlog administration are already unsupported server topology
surfaces. Supported DDL, DML, transactions, crash/reopen behavior, native
storage engines, diagnostics, JSON, GEOMETRY/GIS, collations, and prepared
statements remain in scope.

## Directory And Lifecycle Impact

The slice only reduces inherited binlog startup and cleanup work. It does not
add durable files or temporary companions. Existing server-surface coverage
continues to verify that binlog and relay-log sidecars are absent.

## Binary-Size Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build build` and
`tools/mariadb-embedded-build measure`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| Dynamic plugin loading trimmed | 26,623,920 bytes / 25.39 MiB | 705 | baseline |
| Binlog injector root trimmed | 26,609,024 bytes / 25.38 MiB | 704 | -14,896 bytes / -1 member |

The pre-strip archive moved from 27,195,584 bytes to 27,180,312 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `rpl_injector.cc.o` and `rpl_record.cc.o` are absent from the measured
  archive.
- `gtid_index.cc.o`, `log_event.cc.o`, and `log_event_server.cc.o` remain in
  the measured archive because retained MariaDB code still references them.
- Replication and binlog SQL remain rejected, `@@log_bin=0` remains covered,
  and no binlog or relay-log sidecars are created.
- Native storage, DDL/DML, transactions, prepared statements, JSON,
  GEOMETRY/GIS, and ordinary diagnostics remain covered by the test suite.
- The measured archive size and member count are recorded in the build and
  size-profile docs.

## Risks

- Some retained MariaDB code still includes `log_event.h` types. The trim must
  not remove declarations needed by generic SQL, transaction, or handler code.
- The no-binlog profile still retains `log.cc`, `gtid_index.cc`,
  `log_event.cc`, `log_event_server.cc`, `sql_binlog.cc`, `sql_repl.cc`,
  `rpl_gtid.cc`, and replication utility files where other retained MariaDB
  paths still reference them. Further cuts need separate link evidence.
