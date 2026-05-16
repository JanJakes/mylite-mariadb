# Log Event Server Trim

## Problem

MyLite rejects SQL `BINLOG` and replication commands before MariaDB execution,
and the default embedded profile already omits MariaDB's primary binlog event
core (`log_event.cc`), GTID index, row-record logging, and injector objects.
Before this slice, `log_event_server.cc` still remained in the default embedded
archive because retained MariaDB code referenced a few shared helpers and
disabled SQL `BINLOG` symbols.

That object carries server-side event serialization, table-map, row-event, and
replication apply code for a server topology feature that MyLite does not
support in the core embedded runtime.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documents binary logging as the server log of data changes used for
  replication and recovery:
  <https://mariadb.com/docs/server/server-management/server-monitoring-logs/binary-log>.
- `mariadb/sql/sql_binlog.cc` implements the SQL `BINLOG` statement by decoding
  base64 events, validating event types through `check_event_type()`, parsing
  events with `Log_event::read_log_event()`, and applying them through
  replication event paths.
- `mariadb/sql/sql_binlog.cc` still references
  `Format_description_log_event::Format_description_log_event(uint8, ...)`,
  `Log_event::get_type_str()`, and
  `Log_event::read_log_event(const uchar *, uint, ...)` even though public
  MyLite direct and prepared SQL policy rejects `BINLOG` before this execution
  path.
- `mariadb/sql/log_event_server.cc` defines those symbols along with many
  server-side event writer, table-map, row-event, and apply routines. It also
  defines shared SQL string-rendering helpers such as `append_query_string()`.
- `mariadb/libmysqld/mylite_log_event_core_stub.cc` already provides the
  retained string-rendering helpers, binlog checksum typelib, and no-binlog
  stubs needed after `log_event.cc` is omitted.
- The default embedded archive currently contains both
  `mylite_log_event_core_stub.cc.o` and `log_event_server.cc.o`.

## Design

- Keep `MYLITE_WITH_BINLOG_CORE` as the disabled-profile boundary. When it is
  `ON`, preserve the upstream-style MariaDB source set.
- When `MYLITE_WITH_BINLOG_CORE=OFF`, remove `../sql/log_event_server.cc` from
  `SQL_EMBEDDED_SOURCES` along with the already omitted `log_event.cc`,
  `gtid_index.cc`, `rpl_injector.cc`, and `rpl_record.cc`.
- Add `mylite_log_event_server_disabled.cc` to provide the minimal
  link-visible `Log_event` and `Format_description_log_event` symbols that
  `sql_binlog.cc` still references.
- Keep `append_query_string()` and `str_to_hex()` in the MyLite stub because
  ordinary SQL rendering paths outside BINLOG still use them.
- Keep the string-rendering helper and disabled event-class symbols in separate
  archive objects so first-party embedded smoke binaries do not pull event
  class vtables through ordinary SQL string rendering.
- Make raw embedded SQL `BINLOG` paths fail closed: event parsing returns
  `NULL` with an error string, and the caller reports a syntax/unsupported
  error instead of applying decoded events.
- Public `libmylite` direct and prepared SQL policy remains the supported
  rejection boundary for user-facing BINLOG behavior.

## Affected Subsystems

- MariaDB embedded build source selection.
- Retained `sql_binlog.cc` link roots.
- No-binlog helper stubs.
- Server-surface compatibility tests and documentation.
- Embedded size reporting.

## MySQL/MariaDB Compatibility Impact

MariaDB Server supports binary logs, SQL `BINLOG`, and replication event replay.
MyLite's core embedded runtime does not. This slice keeps MyLite's existing
out-of-scope compatibility status: representative direct and prepared
replication/binlog SQL remains rejected before MariaDB execution.

Raw MariaDB embedded callers that bypass `libmylite` may still reach
`sql_binlog.cc`; in the disabled profile those paths parse no events and fail
closed rather than applying binlog data.

## DDL Metadata Routing Impact

None. Binlog event replay and replication metadata are server topology features,
not MyLite catalog DDL.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companion files. Omitting
`log_event_server.cc` reduces retained server binlog event code and keeps
binlog, relay-log, and replication event replay outside the MyLite file
lifecycle.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Direct and prepared SQL
`BINLOG` rejection continues to use existing MyLite diagnostics.

## Storage-Engine Routing Impact

None. This slice does not change requested-engine routing or MyLite handler
behavior.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol adapters should inherit the same core policy and reject
replication/binlog administration unless a separate server-topology package is
designed.

## Binary-Size Impact

The archived bundle-size research ranks omitting `log_event_server.cc.o` as a
large archive win and a small linked-runtime win because most code is not pulled
into first-party embedded test binaries. The implemented default embedded
archive replaces `log_event_server.cc.o` with
`mylite_log_event_server_disabled.cc.o` and measures 26,674,872 bytes /
25.44 MiB with 670 members. That is 102,088 bytes smaller than the previous
documented default archive baseline.

The current opt-in storage-smoke archive also replaces
`log_event_server.cc.o` with `mylite_log_event_server_disabled.cc.o` and
measures 26,870,264 bytes / 25.63 MiB with 673 members. Its total size includes
current static MyLite handler objects, so compare it only against matching
storage-smoke sources.

## License And Dependency Impact

No new dependency and no license change. The stub is first-party
GPL-2.0-compatible code inside the MariaDB-derived tree.

## Test And Verification Plan

- Keep direct and prepared SQL coverage proving representative `BINLOG`
  statements remain rejected before MariaDB execution.
- Confirm the default and storage-smoke embedded archives contain
  `mylite_log_event_core_stub.cc.o` and
  `mylite_log_event_server_disabled.cc.o`, and omit
  `log_event_server.cc.o`.
- Build and measure the default embedded profile and storage-smoke profile.
- Run `dev`, `embedded-dev`, and `storage-smoke-dev` build/test presets.
- Run the server-surface compatibility report, size report, format, tidy, shell
  syntax, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with `MYLITE_WITH_BINLOG_CORE=OFF`
  without `log_event_server.cc.o` in the archive.
- Public direct/prepared BINLOG behavior remains rejected by MyLite SQL policy.
- Raw retained embedded BINLOG paths fail closed and do not apply events.
- Size measurements and compatibility documentation are updated.

## Implementation Result

- `MYLITE_WITH_BINLOG_CORE=OFF` now removes `log_event_server.cc` from the
  embedded source list.
- `mylite_log_event_core_stub.cc` remains the minsize home for ordinary SQL
  string-rendering helpers and binlog checksum typelib state.
- `mylite_log_event_server_disabled.cc` owns the disabled `Log_event` and
  `Format_description_log_event` symbols needed by retained `sql_binlog.cc`
  references.
- Direct and prepared SQL `BINLOG` rejection remains covered by existing
  server-surface tests.

## Risks

- `log_event_server.cc` defines many unrelated event classes. The stub should
  stay limited to symbols proven necessary by current link roots rather than
  recreating partial binlog replay behavior.
- Future MariaDB source updates may add new references from retained sources to
  log-event classes; archive member checks and link failures should catch that
  during rebase work.
