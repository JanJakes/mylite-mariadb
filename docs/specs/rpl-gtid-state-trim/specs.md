# RPL GTID State Trim

## Problem

MyLite rejects replication and binlog administration before MariaDB execution,
and the default embedded profile already omits the primary binlog event roots,
GTID index, row-record logging, injector, and log-event server runtime. Before
this slice, `rpl_gtid.cc` still remained in the default embedded archive.

`rpl_gtid.cc` implements MariaDB's replication GTID slave state, GTID wait
queues, GTID parser helpers, GTID state validators, binlog GTID state, and
filter classes. In the current no-binlog embedded profile, retained MariaDB code
only references a small `rpl_binlog_state` lifecycle subset from that object.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/rpl_gtid.h` defines `rpl_binlog_state_base` and
  `rpl_binlog_state`, the in-memory binlog GTID state used by the binary log
  subsystem.
- `mariadb/sql/rpl_gtid.cc` defines the full GTID surface: slave-state table
  maintenance, GTID string parsing, wait queues, binlog-state mutation,
  GTID-state validation, and event-filter classes.
- The current default embedded archive still contains `rpl_gtid.cc.o`.
- `nm -u -C build/mariadb-embedded/libmysqld/libsql_embedded.a` shows retained
  non-`rpl_gtid.cc` references only to
  `rpl_binlog_state::init()`, `reset()`, `free()`,
  `load(rpl_gtid *, uint)`, and `~rpl_binlog_state()`.
- Those references come from retained `mariadb/sql/log.cc` startup/shutdown and
  disabled binlog reset paths. MyLite's disabled profile already compiles
  binlog startup, GTID index, event writes, and SQL `BINLOG` event parsing to
  no-ops or fail-closed stubs.
- `mariadb/sql/sql_plugin.cc` references `opt_gtid_pos_auto_plugins`, but that
  variable is defined by retained embedded runtime state, not by `rpl_gtid.cc`.

## Design

- Keep `MYLITE_WITH_BINLOG_CORE` as the disabled-profile boundary. When it is
  `ON`, preserve MariaDB's `../sql/rpl_gtid.cc`.
- When `MYLITE_WITH_BINLOG_CORE=OFF`, remove `../sql/rpl_gtid.cc` from
  `SQL_EMBEDDED_SOURCES`.
- Add `mylite_rpl_gtid_disabled.cc` with the minimal
  `rpl_binlog_state_base` and `rpl_binlog_state` lifecycle symbols required by
  retained no-binlog startup/shutdown code.
- Keep disabled GTID state empty. `init()`, `reset()`, `free()`, and the
  destructor are no-ops, and `load(rpl_gtid *, uint)` accepts the call without
  publishing state because the embedded no-binlog profile has no binary log to
  own GTID history.
- Do not add parser, wait, slave-state table, or filter stubs until link
  evidence shows retained code needs them. Those surfaces remain unsupported and
  rejected by MyLite SQL policy where user-visible.

## Affected Subsystems

- MariaDB embedded build source selection.
- Retained no-binlog `MYSQL_BIN_LOG` startup/shutdown code.
- Replication/binlog compatibility documentation.
- Embedded archive and size reporting.

## MySQL/MariaDB Compatibility Impact

MariaDB Server supports GTID state and replication administration. MyLite's core
embedded runtime does not. This slice keeps replication and binlog out of scope:
representative direct and prepared replication/binlog SQL remains rejected
before MariaDB execution.

Raw retained MariaDB code paths that touch disabled binlog GTID state see an
empty no-op state object instead of a partial GTID implementation.

## DDL Metadata Routing Impact

None. GTID replication state is server topology state, not MyLite catalog DDL.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companion files. The disabled profile keeps
`mysql.gtid_slave_pos` table maintenance, binlog GTID state persistence, and
GTID wait queues outside the MyLite file lifecycle.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

None. This slice does not change requested-engine routing or MyLite handler
behavior.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol or replication-adapter work must not infer GTID support
from the presence of a no-op embedded lifecycle stub.

## Binary-Size Impact

The archived bundle-size research ranks replacing `rpl_gtid.cc` with a small
no-binlog lifecycle stub as a modest linked-runtime win and a larger archive
win. The implemented default embedded archive replaces `rpl_gtid.cc.o` with
`mylite_rpl_gtid_disabled.cc.o` and measures 26,610,000 bytes / 25.38 MiB with
670 members. That is 64,872 bytes smaller than the previous documented default
archive baseline.

The current opt-in storage-smoke archive also replaces `rpl_gtid.cc.o` with
`mylite_rpl_gtid_disabled.cc.o` and measures 26,805,392 bytes / 25.56 MiB with
673 members. Its total size includes current static MyLite handler objects, so
compare it only against matching storage-smoke sources.

## License And Dependency Impact

No new dependency and no license change. The stub is first-party
GPL-2.0-compatible code inside the MariaDB-derived tree.

## Test And Verification Plan

- Keep direct and prepared SQL coverage proving representative replication and
  binlog statements remain rejected before MariaDB execution.
- Confirm the default and storage-smoke embedded archives contain
  `mylite_rpl_gtid_disabled.cc.o` and omit `rpl_gtid.cc.o`.
- Build and measure the default embedded profile and storage-smoke profile.
- Run `dev`, `embedded-dev`, and `storage-smoke-dev` build/test presets.
- Run the server-surface compatibility report, size report, format, tidy, shell
  syntax, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with `MYLITE_WITH_BINLOG_CORE=OFF`
  without `rpl_gtid.cc.o` in the archive.
- Public direct/prepared replication and BINLOG behavior remains rejected by
  MyLite SQL policy.
- Retained no-binlog startup/shutdown paths can initialize, reset, load, free,
  and destroy an empty GTID state object.
- Size measurements and compatibility documentation are updated.

## Implementation Result

- `MYLITE_WITH_BINLOG_CORE=OFF` now removes `rpl_gtid.cc` from the embedded
  source list.
- `mylite_rpl_gtid_disabled.cc` owns the empty no-binlog
  `rpl_binlog_state_base` and `rpl_binlog_state` lifecycle symbols needed by
  retained `log.cc` references.
- Default and storage-smoke archives contain `mylite_rpl_gtid_disabled.cc.o`
  and omit `rpl_gtid.cc.o`.
- Direct and prepared replication/BINLOG rejection remains covered by existing
  server-surface tests.

## Risks

- Future MariaDB source updates may add retained references to more GTID helper
  functions. Link failures and archive symbol checks should catch that during
  upstream rebase work.
- `rpl_gtid.h` still exposes many GTID classes to retained headers. This slice
  removes the runtime object from the disabled profile, not the public internal
  declarations.
