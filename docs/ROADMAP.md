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
one primary `.mylite` file. Durable application state lives in MyLite storage,
not in a MariaDB datadir or existing engine sidecars.

## Implementation Plan

| Order | Slice | Status | Purpose |
| --- | --- | --- | --- |
| 0 | Project foundation | ✅&nbsp;Done | Define the product goal, GPL baseline, architecture direction, workflow, and initial MariaDB research. |
| 1 | Import MariaDB 11.8.6 | ✅&nbsp;Done | Import `mariadb-11.8.6` mechanically and record upstream refs. |
| 2 | Minimal embedded build | ✅&nbsp;Done | Produce a reproducible embedded build and record baseline artifact size and enabled components. |
| 3 | Embedded bootstrap | ✅&nbsp;Done | Start the MariaDB-derived runtime under MyLite-owned defaults and reject daemon-only startup surfaces. |
| 4 | Public open/close API | ✅&nbsp;Done | Add `libmylite` database handles, diagnostics, open flags, and close behavior. |
| 5 | SQL execution API | 🟡&nbsp;In&nbsp;progress | Add direct execution, prepared statements, bindings, columns, warnings, affected rows, and insert ids. Direct execution is implemented; prepared statements and typed values remain. |
| 6 | Storage engine skeleton | ✅&nbsp;Done | Register a static MyLite storage engine with controlled handler smoke coverage. |
| 7 | File header and empty catalog | ✅&nbsp;Done | Create/open a valid `.mylite` file with a versioned header and empty catalog. |
| 8 | MyLite metadata DDL and discovery | 🟡&nbsp;In&nbsp;progress | Store explicit `ENGINE=MYLITE` table definitions in the catalog and discover them without durable `.frm` sidecars. |
| 9 | Sidecar lifecycle gates | ⚪&nbsp;Planned | Detect and reject persistent MariaDB engine sidecars around metadata DDL, close/reopen, and failed-create cleanup. |
| 10 | Engine routing policy | ⚪&nbsp;Planned | Record requested engine vs. effective MyLite engine and route omitted/default, `InnoDB`, `MyISAM`, and `Aria` metadata where safe. |
| 11 | Row and index storage | ⚪&nbsp;Planned | Implement table scans, row DML, primary/secondary indexes, uniqueness, autoincrement, BLOB/TEXT overflow, and copy `ALTER` rebuilds. |
| 12 | Transactions and recovery | ⚪&nbsp;Planned | Add atomic publication, rollback, savepoints, crash recovery, checksums, and companion-file lifecycle tests. |
| 13 | Locking and concurrency | ⚪&nbsp;Planned | Add safe file locks, multiple-reader behavior, and a storage design that preserves concurrent writer goals. |
| 14 | Compatibility harness | ⚪&nbsp;Planned | Run embedded lifecycle, sidecar detection, MariaDB comparison, crash/reopen, and application-query coverage in repeatable groups. |
| 15 | Application schemas | ⚪&nbsp;Planned | Test representative application schemas, including WordPress-shaped DDL and common MySQL/MariaDB ORM output. |
| 16 | Server-surface policy | ⚪&nbsp;Planned | Explicitly reject or replace users/auth, replication/binlog, dynamic plugins, events, performance schema, and external durable engines. |
| 17 | Size profile hardening | ⚪&nbsp;Planned | Trim daemon-only and low-value optional components after the embedded runtime and storage shape are measurable. |

## Size And Profile Direction

MyLite should be smaller than a full MariaDB server distribution, but size is a
measured engineering constraint. The default embedded profile omits runtime
surface that does not fit a local file-owned library:

- network listener and server account administration,
- replication, binlog, relay log, and Galera/wsrep,
- dynamic plugin loading and external durable storage engines,
- performance schema and server audit plugins,
- rarely used optional engines or plugins unless a slice justifies them.

The minimal embedded build establishes the first baseline. Later slices record
meaningful size changes when they add or remove runtime surface.

Historical branch-level size research is archived in
[Bundle size reduction attempts](architecture/bundle-size-research.md). Treat
it as ranked evidence for size-profile work; rerun candidates against the
current baseline before accepting them.
