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
- optional Oracle SQL compatibility mode,
- optional fmtlib-backed SQL helpers such as `SFORMAT()`,
- legacy diagnostic SELECT procedures such as `PROCEDURE ANALYSE()`,
- rarely used optional engines or plugins unless a slice justifies them.

The minimal embedded build establishes the first baseline. Later slices record
meaningful size changes when they add or remove runtime surface.
The first safe hardening steps use size-oriented release flags, strip debug and
local-symbol metadata from the embedded static archive, and omit unused
Performance Schema and Feedback static plugins. Server help-table lookup is
stubbed only after policy coverage proves `HELP` is outside the embedded
application SQL profile. Statement profiling is disabled after policy coverage
proves profiling SQL is a server diagnostic surface, not application data
behavior. The query cache is stubbed after policy coverage proves query-cache
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
core library contract; shared log/event objects that other MariaDB code still
references remain for later, narrower review.
Legacy `PROCEDURE ANALYSE()` support is omitted after policy coverage proves it
is an obsolete diagnostic SELECT extension rather than application data
functionality; normal SELECT execution remains supported.
Compatibility-sensitive code removals require separate evidence before they
are accepted.

Historical branch-level size research is archived in
[Bundle size reduction attempts](architecture/bundle-size-research.md). Treat
it as ranked evidence for size-profile work; rerun candidates against the
current baseline before accepting them.
