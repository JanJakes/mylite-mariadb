# Binlog Core Runtime Trim

## Problem Statement

MyLite's core library is serverless and starts embedded MariaDB with
`--skip-log-bin`. Replication and binlog command families are already rejected
by the server-surface policy, but the default embedded archive still retained
active binlog transaction, row-event, GTID-state, and mandatory plugin entry
points.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` passes `--skip-log-bin`,
  `--skip-slave-start`, and rejects replication/binlog SQL before dispatch.
- `packages/libmylite/tests/embedded_server_surface_policy_test.c` verifies
  `@@log_bin=0` and absence of binlog and relay-log sidecars.
- `mariadb/libmysqld/CMakeLists.txt` included `../sql/rpl_record.cc` in the
  embedded archive even though row-event replay is outside the MyLite core.
- `mariadb/sql/handler.cc`, `mariadb/sql/log.cc`,
  `mariadb/sql/sql_class.cc`, and `mariadb/sql/sql_builtin.cc.in` retain
  binlog transaction participant hooks, row-event writing paths, GTID-state
  accessors, and mandatory binlog plugin registration.

## Design

- Add `MYLITE_WITH_BINLOG_CORE`, defaulting to `ON`.
- Set `MYLITE_WITH_BINLOG_CORE=OFF` only in the MyLite embedded baseline.
- When disabled, omit `rpl_record.cc` from `sql_embedded`, skip mandatory
  binlog plugin registration, and compile binlog transaction, row-event,
  GTID-state, event-write, and table-map entry points to no-op or fail-closed
  behavior.
- Keep the normal MariaDB server build path unchanged.
- Keep shared log/event objects that retained MariaDB code still references.
  A later binlog injector-root trim omits the separable injector root once
  link evidence proves it is no longer needed in the embedded no-binlog
  profile.

## Compatibility Impact

Replication and binary logging remain out of scope for the core API. Supported
DDL, DML, transactions, crash/reopen behavior, native storage, diagnostics,
warnings, and prepared statements must keep working. The trim does not remove
JSON, GEOMETRY/GIS, collations, native storage engines, or built-in SQL
functions.

## Directory And Lifecycle Impact

The embedded profile must not create binlog, relay-log, GTID-index, or
binlog-cache sidecars. Existing server-surface coverage remains the directory
boundary evidence.

## Binary-Size Impact

Measured on 2026-05-20 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta from UDF baseline |
| --- | ---: | ---: | ---: |
| Dynamic UDF baseline | 27,337,960 bytes / 26.07 MiB | 706 | baseline |
| No embedded binlog core | 27,265,728 bytes / 26.00 MiB | 705 | -72,232 bytes / -1 member |

The pre-strip archive moved from 27,938,032 bytes to 27,864,688 bytes.

## Acceptance Criteria

- `MYLITE_WITH_BINLOG_CORE=OFF` appears in the embedded CMake cache.
- `rpl_record.cc.o` is absent from `libmariadbd.a`.
- Replication/binlog SQL remains rejected and `@@log_bin=0` remains covered.
- No binlog or relay-log sidecars are created by the test suite.
- `libmylite` links and the full `dev` and `embedded-dev` test suites pass.
- Build, format, tidy, diff, and size checks pass.

## Risks

`log.cc`, log-event helpers, GTID-state helpers, replication utility files, and
binlog plugin symbols remain in the archive because other retained MariaDB code
still references them. Further binlog/event pruning needs a separate source
review and link evidence.
