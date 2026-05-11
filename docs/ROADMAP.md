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
| 1 | Import MariaDB 11.8.6 | ⚪&nbsp;Planned | Import `mariadb-11.8.6` mechanically and record upstream refs. |
| 2 | Minimal embedded build | ⚪&nbsp;Planned | Produce a reproducible embedded build and record baseline artifact size and enabled components. |
| 3 | Embedded bootstrap | ⚪&nbsp;Planned | Start the MariaDB-derived runtime under MyLite-owned defaults and reject daemon-only startup surfaces. |
| 4 | Public open/close API | ⚪&nbsp;Planned | Add `libmylite` database handles, diagnostics, open flags, and close behavior. |
| 5 | Direct SQL execution | ⚪&nbsp;Planned | Add one-shot SQL execution so embedded storage smoke tests can run SQL. |
| 6 | Native storage baseline | ⚪&nbsp;Planned | Configure MariaDB native storage under the MyLite database directory and prove controlled DDL/DML persistence across reopen. |
| 7 | Metadata and DDL lifecycle | ⚪&nbsp;Planned | Keep controlled `CREATE`, `ALTER`, `DROP`, and `RENAME` metadata and engine files inside the MyLite database directory. |
| 8 | Directory lifecycle policy | ⚪&nbsp;Planned | Define database-directory layout, initialization markers, existing-directory policy, cleanup rules, and version policy. |
| 9 | Native table operations | ⚪&nbsp;Planned | Validate representative row, index, constraint, and copy-alter behavior through native engines. |
| 10 | Transactions and recovery | ⚪&nbsp;Planned | Cover transaction commit, rollback, savepoints, clean reopen, crash recovery, and representative engine companions. |
| 11 | Locking and concurrency | ⚪&nbsp;Planned | Prove directory locking and document planned multiple-reader and concurrent-writer modes. |
| 12 | Compatibility harness | ⚪&nbsp;Planned | Run embedded lifecycle, directory-boundary detection, MariaDB-reference, crash/reopen, application-query, and query-surface coverage in repeatable CTest label groups. |
| 13 | Prepared SQL API | ⚪&nbsp;Planned | Add reusable prepared statements, parameter bindings, typed column access, warning lookup, binary-safe values, reset/finalize behavior, and close-time statement lifetime enforcement. |
| 14 | Engine support and application schemas | ⚪&nbsp;Planned | Test supported explicit `ENGINE=` clauses, default-engine resolution, MEMORY reopen semantics, and representative application-shaped DDL. |
| 15 | Server-surface policy | ⚪&nbsp;Planned | Explicitly reject users/auth, replication/binlog, dynamic plugins, events, and server-owned metadata surfaces. |
| 16 | Size profile hardening | ⚪&nbsp;Planned | Trim daemon-only and low-value optional components after the embedded runtime and storage shape are measurable. |

## Size And Profile Direction

MyLite should be smaller than a full MariaDB server distribution, but size is a
measured engineering constraint. The default embedded profile omits runtime
surface that does not fit a local directory-owned library:

- network listener and server account administration,
- replication, binlog, relay log, and Galera/wsrep,
- dynamic plugin loading and external durable storage engines,
- performance schema and server audit plugins,
- rarely used optional engines or plugins unless a slice justifies them.

The minimal embedded build establishes the first baseline. Later slices record
meaningful size changes when they add or remove runtime surface.
