# Event Parse Data Trim

## Problem

MyLite rejects event scheduler activation and event DDL before MariaDB
execution. The default embedded archive no longer carries MariaDB's event
scheduler, queue, repository, or event object runtime. Before this slice, it
still linked `event_parse_data.cc`.

That object validates `CREATE EVENT` and `ALTER EVENT` schedule expressions,
definers, dates, and replicated-event state for a feature MyLite does not
support. The embedded parser still needs the `Event_parse_data` type so raw
embedded event statements can parse far enough to fail closed, but the full
validation body is not useful in the MyLite profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documents events as named database objects containing SQL statements
  executed later by the event scheduler:
  <https://mariadb.com/docs/server/server-usage/triggers-events/event-scheduler/events>.
- MariaDB documents `CREATE EVENT` as scheduling SQL execution and requiring an
  event name, an `ON SCHEDULE` clause, and a `DO` clause:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/create-event>.
- `mariadb/sql/sql_yacc.yy` allocates `Event_parse_data` while parsing
  `CREATE EVENT` and `ALTER EVENT`, stores schedule/body fields in the object,
  and sets `SQLCOM_CREATE_EVENT` / `SQLCOM_ALTER_EVENT`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_EVENT`,
  `SQLCOM_ALTER_EVENT`, `SQLCOM_DROP_EVENT`, `SQLCOM_SHOW_CREATE_EVENT`, and
  `SQLCOM_SHOW_EVENTS` through `Events` only when `HAVE_EVENT_SCHEDULER` is
  defined; embedded builds without that define report unsupported embedded
  event behavior.
- `mariadb/libmysqld/CMakeLists.txt` already excludes `events.cc`,
  `event_scheduler.cc`, `event_queue.cc`, `event_db_repository.cc`, and
  `event_data_objects.cc` from `SQL_EMBEDDED_SOURCES`, but still includes
  `../sql/event_parse_data.cc`.
- `mariadb/sql/event_parse_data.cc` evaluates event schedule expressions,
  copies definers, checks past dates, and handles replicated-event originator
  state. Those bodies are only meaningful for supported event DDL.
- Before this slice, the default embedded archive contained
  `event_parse_data.cc.o` as the only event-specific archive member.

## Design

- Add `MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION`, defaulting to `ON` for
  upstream-style embedded builds.
- Force `MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When enabled, keep MariaDB's existing `../sql/event_parse_data.cc`.
- When disabled, build a small embedded-only
  `mylite_event_parse_data_disabled.cc` instead.
- The disabled stub preserves the link-visible `Event_parse_data` constructor,
  allocator, and validation methods used by the generated parser and retained
  raw embedded SQL paths.
- The constructor must initialize all fields the parser writes or reads.
- `check_parse_data()` and `check_dates()` must fail closed with
  `ER_NOT_SUPPORTED_YET` because event DDL has no MyLite catalog, scheduler,
  privilege, or runtime design.
- Public `libmylite` direct and prepared SQL policy remains the supported
  user-visible rejection boundary for event DDL.

## Affected Subsystems

- MariaDB embedded build source selection.
- Generated MariaDB parser event actions that allocate and populate
  `Event_parse_data`.
- Public direct/prepared SQL server-surface and non-table-object tests.
- Embedded build, size profile, compatibility matrix, and roadmap docs.

## MySQL/MariaDB Compatibility Impact

MariaDB Server supports events and event scheduler metadata. MyLite does not
support that server scheduler surface in the current embedded core. Event DDL
and scheduler activation remain explicit unsupported behavior through
`libmylite`.

Raw MariaDB embedded callers that bypass MyLite's SQL policy still get a
fail-closed unsupported diagnostic instead of full event parse-data validation.

## DDL Metadata Routing Impact

No event metadata is added. This slice prevents retained event parse validation
from becoming a partial implementation of event catalog behavior before MyLite
has a catalog-backed non-table object design.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companion files. The disabled profile keeps
event scheduler state, event repository loading, and event metadata sidecars
outside the embedded runtime.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Direct and prepared event DDL
continue to fail with MyLite's unsupported SQL policy.

## Storage-Engine Routing Impact

None. Events are non-table database objects and do not route through the MyLite
handler.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol or adapter layers should inherit the same event policy from
the core library. Event scheduler support needs a separate product design if it
is ever added.

## Binary-Size Impact

The archived bundle-size research ranks replacing full event parse-data
validation with a minimal parser-allocation stub as a small linked-runtime win
and an archive win. The implemented default embedded archive omits
`event_parse_data.cc.o`, contains `mylite_event_parse_data_disabled.cc.o`,
and measures 26,776,960 bytes / 25.54 MiB with 670 members. That is 5,536
bytes smaller than the previous documented default archive baseline.

The current opt-in storage-smoke archive also contains
`mylite_event_parse_data_disabled.cc.o` instead of `event_parse_data.cc.o` and
measures 26,972,352 bytes / 25.72 MiB with 673 members. Its total size includes
current static MyLite handler objects, so compare it only against matching
storage-smoke sources.

## License And Dependency Impact

No new dependency and no license change. The stub is first-party
GPL-2.0-compatible code inside the MariaDB-derived tree.

## Test And Verification Plan

- Keep direct SQL coverage proving representative `CREATE EVENT` remains
  rejected.
- Add prepared SQL coverage for representative `CREATE EVENT` rejection before
  MariaDB execution.
- Confirm the default and storage-smoke embedded archives contain
  `mylite_event_parse_data_disabled.cc.o` and omit `event_parse_data.cc.o`.
- Build and measure the default embedded profile and storage-smoke profile.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility report, size report, format, tidy, shell
  syntax, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION=OFF`.
- Public direct/prepared event DDL remains rejected before MariaDB execution.
- The generated parser can still link and raw embedded event parse paths fail
  closed.
- The embedded archives replace `event_parse_data.cc.o` with the MyLite stub.
- Size measurements and compatibility documentation are updated.

## Implementation Result

- `MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION` defaults to `ON` for
  upstream-style embedded builds and is forced `OFF` by MyLite's embedded
  baseline profile.
- `mariadb/libmysqld/mylite_event_parse_data_disabled.cc` keeps parser
  allocation/linkage available while `check_parse_data()` and `check_dates()`
  fail closed with `ER_NOT_SUPPORTED_YET`.
- Direct event DDL remains covered by server-surface policy tests, and prepared
  `CREATE EVENT` rejection is covered in `embedded_statement_test.c`.
- Default and storage-smoke archives contain
  `mylite_event_parse_data_disabled.cc.o` and omit `event_parse_data.cc.o`.

## Risks

- The generated parser writes directly into `Event_parse_data` fields. The stub
  constructor must keep those fields initialized consistently with MariaDB's
  original constructor.
- Full removal of the class is not worth the parser churn; the bounded target
  is removing validation/runtime roots, not changing the grammar.
