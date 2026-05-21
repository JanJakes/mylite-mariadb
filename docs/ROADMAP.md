# MyLite Roadmap

This roadmap orders the first engineering slices. It tracks product work at a
higher level than per-slice specs.

## Status Key

- ✅&nbsp;Done: accepted and represented in the repository.
- ⚪&nbsp;Planned: expected work, but not started.
- 🟡&nbsp;In&nbsp;progress: active implementation or research.
- ❌&nbsp;Blocked: waiting on a named prerequisite or decision.

## Current Direction

MyLite starts from MariaDB 11.8 LTS and exposes an embedded `libmylite` API over
one MyLite-owned database directory. Durable application state lives in MariaDB
native storage files inside that directory.

## Implementation Plan

| Order | Slice | Status | Purpose |
| --- | --- | --- | --- |
| 0 | Project foundation | ✅&nbsp;Done | Define the product goal, GPL baseline, architecture direction, workflow, and initial MariaDB research. |
| 1 | Import MariaDB 11.8.6 | ✅&nbsp;Done | Import `mariadb-11.8.6` mechanically and record upstream refs. |
| 2 | Minimal embedded build | ✅&nbsp;Done | Produce a reproducible embedded build and record baseline artifact size and enabled components. |
| 3 | Embedded bootstrap | ✅&nbsp;Done | Start the MariaDB-derived runtime under MyLite-owned defaults and reject daemon-only startup surfaces. |
| 4 | Public open/close API | ✅&nbsp;Done | Add `libmylite` database handles, diagnostics, open flags, and close behavior. |
| 5 | Direct SQL execution | ✅&nbsp;Done | Add one-shot SQL execution, textual result callbacks, diagnostics, affected rows, and insert ids so embedded storage smoke tests can run SQL. |
| 6 | Native storage baseline | ✅&nbsp;Done | Configure MariaDB native storage under the MyLite database directory and cover controlled MyISAM DDL/DML persistence across reopen. |
| 7 | Metadata and DDL lifecycle | ✅&nbsp;Done | Keep controlled MyISAM `CREATE`, `ALTER`, `DROP`, and `RENAME` metadata and engine files inside the MyLite database directory. |
| 8 | Directory lifecycle policy | ✅&nbsp;Done | Define database-directory layout, initialization markers, existing-directory policy, cleanup rules, and version policy. |
| 9 | Native table operations | ✅&nbsp;Done | Validate table scans, row DML, primary/secondary indexes, uniqueness, autoincrement, BLOB/TEXT overflow, and copy `ALTER` rebuilds through native engines. |
| 10 | Transactions and recovery | ✅&nbsp;Done | Enable explicit InnoDB tables under the MyLite directory and cover commit, rollback, savepoints, clean reopen, child-process recovery, and representative engine companions. |
| 11 | Locking and concurrency | ✅&nbsp;Done | Add exclusive cross-process directory locking for read/write opens, preserve live runtime state on failed lock attempts, and document planned multiple-reader and concurrent-writer modes. |
| 12 | Compatibility harness | ✅&nbsp;Done | Run embedded lifecycle, directory-boundary detection, MariaDB-reference, crash/reopen, application-query, and query-surface coverage in repeatable CTest label groups. |
| 13 | Prepared SQL API | ✅&nbsp;Done | Add reusable prepared statements, parameter bindings, typed column access, warning lookup, binary-safe values, reset/finalize behavior, and close-time statement lifetime enforcement. |
| 14 | Engine support and application schemas | ✅&nbsp;Done | Test supported explicit `ENGINE=` clauses, default-engine resolution, MEMORY reopen semantics, and representative WordPress-shaped InnoDB DDL. |
| 15 | Server-surface policy | ✅&nbsp;Done | Explicitly reject users/auth, replication/binlog, dynamic plugins, events, server-owned metadata, SQL help, profiling, and query-cache management, while disabling server topology features at startup. |
| 16 | Size profile hardening | 🟡&nbsp;In&nbsp;progress | Trim safe archive/package overhead first, then evaluate daemon-only and low-value optional components after the embedded runtime and storage shape are measurable. |

## Size And Profile Direction

MyLite should be smaller than a full MariaDB server distribution, but size is a
measured engineering constraint. The default embedded profile omits runtime
surface that does not fit a local directory-owned library:

- network listener and server account administration,
- replication, binlog, relay log, and Galera/wsrep,
- dynamic plugin loading and external durable storage engines,
- dynamic UDF shared-library loading,
- performance schema, statement profiling, query cache, and server audit plugins,
- optimizer trace diagnostics,
- general and slow query logs,
- statement digest diagnostics,
- server status variables,
- process-list metadata,
- user statistics diagnostics,
- optional Oracle SQL compatibility mode,
- optional Oracle compatibility function aliases,
- full inherited server diagnostic text,
- optional fmtlib-backed SQL helpers such as `SFORMAT()`,
- legacy diagnostic SELECT procedures such as `PROCEDURE ANALYSE()`,
- server help text such as system-variable comments,
- static server-information `SHOW` commands,
- command-line option help prose,
- startup options for disabled server topology and dynamic plugin loading,
- inherited binary-log cache directory setup,
- row-replication type conversion,
- binary-log event parser and reader runtime,
- host-file SELECT exports,
- rarely used optional engines or plugins unless a slice justifies them.

The minimal embedded build establishes the first baseline. Later slices record
meaningful size changes when they add or remove runtime surface.
The first safe hardening steps use size-oriented release flags, strip debug and
local-symbol metadata from the embedded static archive, and omit unused
Performance Schema and Feedback static plugins. Server help-table lookup is
stubbed only after policy coverage proves `HELP` is outside the embedded
application SQL profile. Statement profiling and its Information Schema
metadata are disabled after policy coverage proves profiling SQL is a server
diagnostic surface, not application data behavior. The query cache is stubbed
after policy coverage proves query-cache
management is a server tuning surface and `SQL_CACHE` / `SQL_NO_CACHE` can
remain no-op parser hints. The optional Oracle SQL-mode parser is replaced with
an unsupported embedded stub after policy coverage proves normal SQL modes and
user variables remain unaffected. The optional `SFORMAT()` helper is omitted
from the embedded function registry after coverage proves direct and prepared
calls fail predictably and ordinary `FORMAT()` remains available; that lets the
embedded SQL target compile without C++ exceptions or unwind tables.
Dynamic UDF shared-library loading is omitted after policy coverage proves
`CREATE FUNCTION ... SONAME` fails predictably; stored functions remain a
separate application SQL surface.
The binary-log transaction/event core is compiled to embedded no-op paths after
policy coverage proves replication and binlog command families are outside the
core library contract. Embedded no-binlog startup, open, cleanup, and
GTID-index update paths are guarded, and the unsupported injector root is
omitted after link evidence proves it is only needed by the server topology
runtime. Shared replication utility helpers still referenced by retained
MariaDB code remain for narrower review.
SQL `BINLOG` statement replay is omitted after coverage proves it is an
unsupported replication replay surface and the embedded dispatcher already
fail-closes it.
Server-side binary-log event writers are replaced with a disabled embedded
source after link evidence proves the only ordinary SQL helper that must remain
is `append_query_string()`.
Binary-log event parser and reader runtime is omitted after link evidence
proves retained embedded paths need only minimal fail-closed event symbols, not
binary-log replay/decode support.
Replication GTID-state runtime is replaced with empty embedded state after link
evidence proves retained no-binlog paths only need the state contract, not
server topology behavior. GTID helper SQL functions are rejected by policy.
Binary-log GTID-index runtime and tuning variables are omitted after source and
link evidence proves they only serve unsupported binlog lookup and replication
positioning paths.
SQL `HANDLER` command runtime is replaced with a disabled embedded source after
source review proves top-level `HANDLER ...` commands are a server table-cursor
surface, not the storage-engine handler abstraction needed by normal table
execution.
`SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` host-file writers are
omitted after coverage proves they are server filesystem export surfaces;
ordinary result delivery and `SELECT ... INTO @variable` remain covered.
Startup option rows for disabled server topology and dynamic-plugin-loading
surfaces are omitted after source review proves the retained `libmylite`
startup vector still uses only serverless, directory-owned options.
Inherited binary-log cache directory setup is skipped when
`MYLITE_WITH_BINLOG_CORE=OFF`; the no-binlog embedded profile does not create
or clean up `#binlog_cache_files`.
Replication execution, slave protocol, checksum, replication-event, and
semi-sync system variables are also omitted after policy coverage proves those
configuration rows belong to unsupported topology behavior; compatibility
variables such as `@@log_bin=0` remain covered. Replication and binlog filter
runtime is omitted after policy coverage proves the inherited filter knobs are
topology configuration, while retained runtime checks behave as if no filters
are configured.
Row-replication type-conversion helpers are replaced with fail-closed embedded
stubs after source review proves row-event apply is already outside the core
profile; ordinary SQL type conversion, native storage, JSON, GEOMETRY/GIS, and
sequence support remain linked through their normal non-replication paths.
Residual replication helper objects are omitted after link evidence proves the
no-binlog embedded profile no longer references `slave.cc`, `sql_repl.cc`,
`rpl_utility.cc`, or `rpl_reporting.cc`.
Legacy `PROCEDURE ANALYSE()` support is omitted after policy coverage proves it
is an obsolete diagnostic SELECT extension rather than application data
functionality; normal SELECT execution remains supported.
System-variable help comments are omitted after coverage proves variable rows
and values remain queryable and only
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` becomes empty.
Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` are omitted
after coverage proves ordinary supported `SHOW` surfaces such as `SHOW
VARIABLES` remain available.
Command-line option help prose is omitted after measurement proves startup
option parsing remains intact and only inherited `--help` descriptions become
empty in the embedded archive.
Optimizer trace diagnostics are omitted after coverage proves trace variables
and `INFORMATION_SCHEMA.OPTIMIZER_TRACE`, qualified or current-schema, are
explicitly unsupported surfaces while ordinary planning, execution, and
`EXPLAIN` remain available.
Persistent optimizer-statistics storage and JSON histogram storage are omitted
after coverage proves `ANALYZE TABLE ... PERSISTENT FOR ...` and statistic
system-variable changes are server-owned tuning surfaces, while ordinary
engine statistics, planning, execution, `ANALYZE TABLE`, and `EXPLAIN` remain
available.
General and slow query logs are omitted after coverage proves query-log
configuration is explicitly unsupported while error logging, SQL diagnostics,
warnings, and result metadata remain available.
Statement digest normalization is omitted after measurement proves Performance
Schema digest text and hash diagnostics are outside the default embedded
profile while ordinary parsing, execution, prepared statements, and `EXPLAIN`
remain available.
Server status-variable publication is omitted after measurement proves
`SHOW STATUS` and status Information Schema rows are daemon diagnostics, not
application SQL or storage behavior; direct and prepared status queries keep
stable empty result behavior while ordinary diagnostics remain available.
Process-list metadata is omitted after coverage proves `SHOW PROCESSLIST` and
`SHOW FULL PROCESSLIST` are daemon thread/session inventory surfaces;
`INFORMATION_SCHEMA.PROCESSLIST` remains queryable and returns zero rows.
Foreign-server metadata is omitted after coverage proves `CREATE SERVER`,
`ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` manage server-global
remote connection definitions in `mysql.servers`, not application tables or
native storage inside the MyLite database directory.
External backup runtime is omitted after coverage proves `BACKUP STAGE`,
`BACKUP LOCK`, and `BACKUP UNLOCK` are backup-tool coordination surfaces;
ordinary DDL backup hooks stay inert so application DDL remains unaffected.
VIO TLS transport is omitted from the core embedded archive because TLS belongs
to socket and wire-protocol adapters, not to the in-process database-directory
startup contract; retained SQL crypto functions continue to use OpenSSL Crypto.
PROXY protocol listener support is omitted for the same serverless-core reason;
the core runtime has no socket listener, and `proxy_protocol_networks` is
absent from `SHOW VARIABLES` and `@@` lookup.
User statistics diagnostics are omitted after coverage proves `userstat`,
userstat Information Schema tables, and `FLUSH *_STATISTICS` are optional
server counters rather than application data behavior.
User-variable diagnostics are omitted after coverage proves the
`user_variables` plugin only exposes optional `@variable` introspection and
reset helpers; ordinary `@variable` SQL remains available.
Unix socket server authentication is omitted after coverage proves local
directory opens do not use server auth plugins and account management remains
outside the embedded core.
Event parse-data validation is reduced to a parser-link stub after coverage
proves event DDL, event metadata commands, and scheduler changes are outside
the embedded core.
Oracle compatibility function aliases and `oracle_schema` routing are omitted
after coverage proves the normal MySQL/MariaDB string functions remain
available, Oracle-only aliases fail predictably, and `oracle_schema` no
longer acts as a built-in compatibility schema.
The full server error-message catalog is compacted after coverage proves
MariaDB error numbers, SQLSTATEs, syntax-error messages, duplicate-key
messages, and generic fallback diagnostics remain available.
Dynamic plugin shared-object loading is omitted after policy coverage proves
plugin SQL is outside the core API; static built-in plugins and native storage
engines remain available, and `@@have_dynamic_loading=NO` records the embedded
runtime contract.
Compatibility-sensitive code removals require separate evidence before they
are accepted.

Historical branch-level size research is archived in
[Bundle size reduction attempts](architecture/bundle-size-research.md). Treat
it as ranked evidence for size-profile work; rerun candidates against the
current baseline before accepting them.
