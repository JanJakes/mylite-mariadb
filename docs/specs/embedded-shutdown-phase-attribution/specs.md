# Embedded Shutdown Phase Attribution

## Problem

Production WordPress PHPUnit timing and the embedded performance probe show that
process-isolated tests pay the full MariaDB embedded lifecycle cost on each
child process. Active-runtime reconnect and steady SQL are comparatively cheap,
while ordinary warm open/close remains dominated by the shutdown side of the
embedded lifecycle. The existing `release_mysql_shutdown` counter covers both
`mysql_thread_end()` and `mysql_server_end()`, so it does not identify whether
the remaining cost is thread teardown or server teardown.

This slice adds attribution only. It does not change embedded runtime lifetime,
PHP mysqli behavior, ownerless concurrency behavior, or MariaDB cleanup order.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` `release_runtime()` is the first-party
  MyLite lifecycle boundary that closes the active embedded runtime. Before this
  slice it measured `mysql_thread_end(); mysql_server_end();` as one
  `EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SHUTDOWN_NS` span.
- `mariadb/libmysqld/libmysql.c` `mysql_thread_end()` delegates to
  `my_thread_end()`.
- `mariadb/libmysqld/libmysql.c` `mysql_server_end()` deinitializes client
  plugins and client errors, conditionally ends VIO/SSL state, calls
  `end_embedded_server()` for the embedded build, calls `my_end(0)` when the
  library initialized the runtime, and resets the client-init state.
- `mariadb/libmysqld/lib_sql.cc` `end_embedded_server()` frees the copied
  argument vector, calls `clean_up(0)`, calls `clean_up_mutexes()`, and clears
  `mysql_embedded_init`.
- `mariadb/sql/mysqld.cc` `clean_up()` performs broad server cleanup including
  log cleanup, cache cleanup, table-definition cache shutdown, plugin shutdown,
  handler shutdown, transaction-log close, metadata-lock teardown, key-cache and
  status cleanup, thread scheduler end, library cleanup, client-error cleanup,
  charset cleanup, and related global server-state teardown.

## Design

Add two MyLite-owned embedded open performance counters:

- `EMBEDDED_OPEN_PERF_RELEASE_MYSQL_THREAD_END_NS`
- `EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SERVER_END_NS`

Keep the existing `EMBEDDED_OPEN_PERF_RELEASE_MYSQL_SHUTDOWN_NS` aggregate so
current log readers keep their total shutdown metric. The aggregate now spans
the same two calls as before, while the new counters measure each call
individually.

Update the embedded performance probe's mirrored counter enum and emit both new
fields in the detailed and compact summary output:

- `release_mysql_thread_end`
- `release_mysql_server_end`

## Compatibility Impact

No SQL, C API, PHP mysqli API, storage-engine, wire-protocol, or
database-directory behavior changes. The performance-stat API still exposes the
same raw counter array shape, extended with two counters before the existing
shutdown aggregate. The probe is updated in the same commit to keep the local
mirror synchronized.

## Directory And Lifecycle Impact

No directory layout or lifecycle semantics change. `mysql_thread_end()` and
`mysql_server_end()` run in the same order as before, and `mylite_close()`
continues to wait for the supported embedded shutdown path.

## Native Storage Impact

No native storage behavior changes. InnoDB, MyISAM, Aria, and other MariaDB
cleanup remains owned by MariaDB's existing shutdown path.

## Build And Size Impact

The slice adds two atomic counters and two probe output fields. It introduces no
new dependency and should have negligible binary-size impact.

## Test And Verification Plan

- Build the production embedded performance probe.
- Run a reduced production embedded performance probe and verify the detailed
  and compact output contain `release_mysql_thread_end`,
  `release_mysql_server_end`, and the retained `release_mysql_shutdown`
  aggregate.
- Run the embedded open/close CTest that exercises the raw counter reader.
- Run `format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The new counters compile in the library and the probe enum mirror.
- The probe emits both new phase names without removing the existing aggregate.
- A reduced production probe produces nonnegative measurements for the new
  phases and keeps the aggregate available.
- Documentation marks this as attribution only, not a shutdown optimization.

## Risks And Follow-Up

The raw-array performance-stat ABI still requires the producer and probe enum
mirrors to stay synchronized. This slice keeps the change local and bounded, but
a later cleanup should centralize first-party performance counter definitions if
the counter set keeps growing.

If `release_mysql_server_end` owns the shutdown time, the next performance slice
should instrument or trim MariaDB embedded cleanup subphases such as
`end_embedded_server()`, `clean_up()`, `plugin_shutdown()`, `ha_end()`,
`tdc_deinit()`, and charset/library cleanup with narrow upstream-derived
changes.
