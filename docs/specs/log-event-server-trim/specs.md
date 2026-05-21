# Log Event Server Trim

## Problem

MyLite's embedded profile rejects replication, binary-log administration, and
SQL `BINLOG` replay, but the default archive still carries MariaDB's
server-side binary-log event writer in `sql/log_event_server.cc`.

`log_event_server.cc` is not pure server topology code: it also owns
`append_query_string()`, which ordinary SQL value rendering uses through
`item.cc` and `sql_type.cc`. Removing the object therefore needs a small
retained helper rather than a blind source deletion.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/log_event_server.cc` defines server-side binary-log event
  constructors, event writers, row-event writers, and replication apply helpers.
- `mariadb/sql/log_event_server.cc` also defines `append_query_string()`.
  `mariadb/sql/item.cc` and `mariadb/sql/sql_type.cc` call that helper for SQL
  literal rendering outside binary-log emission.
- A link probe that removed `log_event_server.cc.o` failed on
  `append_query_string()` plus binary-log event writer/vtable symbols retained
  through `log.cc`, `temporary_tables.cc`, and `log_event.cc`.

This spec records the server-event-writer trim boundary. A later dedicated
log-event-parsing trim omits `log_event.cc` too, after moving the remaining
event-reader link contract into the disabled MyLite event source.

## Design

Add `MYLITE_WITH_LOG_EVENT_SERVER`. The default MariaDB build keeps the upstream
source. The MyLite embedded baseline sets the option to `OFF`, replaces
`log_event_server.cc` with `mylite_log_event_server_disabled.cc`, and keeps:

- the real `append_query_string()` escaping behavior;
- minimal constructors needed by retained MariaDB link paths;
- fail-closed event writer and row-event stubs for unsupported binary-log paths.

The stub does not emulate binary-log output. Any retained attempt to write a
binary-log event returns an error-style value, while normal `libmylite` policy
continues to reject public replication and binary-log SQL before dispatch.

## Compatibility Impact

No supported SQL, native storage, JSON, GEOMETRY, transaction, or embedded C API
behavior should change. Replication and binary-log event output remain
unsupported server topology behavior.

`append_query_string()` stays behavior-compatible because ordinary prepared
statement and type rendering still need SQL literal escaping.

## Database Directory And Native Storage Impact

No durable paths, temporary paths, locks, metadata files, or native storage
engine files change. The slice removes server binary-log event emission code
from the embedded archive only.

## Binary Size Impact

Expected impact is mostly archive size: `log_event_server.cc.o` leaves
`libmariadbd.a` and a much smaller MyLite disabled object takes its place. Linked
size impact is expected to be small because the retained executable already
dead-strips most unsupported event-writing paths.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_LOG_EVENT_SERVER=OFF` appears in the embedded CMake
  cache.
- Confirm `log_event_server.cc.o` is absent from `libmariadbd.a` and
  `mylite_log_event_server_disabled.cc.o` is present.
- Run the embedded and default CMake builds and tests.
- Run format, tidy, `git diff --check`, and archive measurement.
- Confirm server-surface policy coverage still rejects replication, binary-log,
  SQL `BINLOG`, and related unsupported server surfaces.

## Acceptance Criteria

- Ordinary SQL execution, prepared statements, native storage, transactions,
  recovery, and compatibility harness tests pass.
- Public unsupported-surface diagnostics remain stable.
- The measured embedded archive size and member count are updated in the build
  docs.
- Documentation records the new trim boundary and keeps `append_query_string()`
  called out as retained ordinary SQL behavior.
