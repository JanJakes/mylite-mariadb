# Embedded Shutdown Cleanup Attribution

## Problem

The embedded shutdown phase attribution slice proved that ordinary warm
open/close shutdown time is effectively all inside `mysql_server_end()`, while
`mysql_thread_end()` is negligible. The remaining bucket is still too coarse to
choose a safe optimization. `mysql_server_end()` includes client cleanup,
embedded-server cleanup, `clean_up()`, mutex teardown, and `my_end(0)`, and
`clean_up()` itself tears down broad MariaDB global server state.

This slice adds attribution only. It does not skip shutdown, change cleanup
order, keep MariaDB global state alive after `mylite_close()`, or change
process-isolated WordPress behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` `release_runtime()` calls
  `mysql_thread_end()` followed by `mysql_server_end()` while closing the last
  active embedded runtime.
- `mariadb/libmysqld/libmysql.c` `mysql_server_end()` gates on
  `mysql_client_init`, then runs `mysql_client_plugin_deinit()`,
  `finish_client_errs()`, optional `vio_end()`, embedded
  `end_embedded_server()`, optional `my_end(0)`, and resets client-init state.
- `mariadb/libmysqld/lib_sql.cc` `end_embedded_server()` frees
  `copy_arguments_ptr`, runs `clean_up(0)`, runs `clean_up_mutexes()`, and
  clears `mysql_embedded_init`.
- `mariadb/sql/mysqld.cc` `clean_up()` has a one-shot `cleanup_done` guard and
  then tears down replication helpers when built, DDL/logging state, plugin
  state, storage handlers, table-definition and metadata-lock state, caches,
  status/accounting globals, scheduler callbacks, MariaDB library state,
  diagnostics/errors, charsets, and final path/list/proxy state.
- Existing MyLite MariaDB-side perf counters use small MyLite-owned headers and
  C-linkage functions, for example
  `mariadb/storage/innobase/include/mylite_ownerless_innodb_deep_perf.h`.

## Design

Add a small MyLite-owned shutdown performance counter API in
`mariadb/include/mylite_embedded_shutdown_perf.h`, implemented from
`mariadb/sql/mysqld.cc` so both C and C++ shutdown files can use C-linkage
helpers without changing build topology.

Instrument these outer phases:

- `mysql_server_end()` total and call count;
- `mysql_client_plugin_deinit()`;
- `finish_client_errs()` from `mysql_server_end()`;
- optional `vio_end()`;
- embedded `end_embedded_server()`;
- optional `my_end(0)`;
- `end_embedded_server()` total and call count;
- argument-vector free inside `end_embedded_server()`;
- `clean_up(0)`;
- `clean_up_mutexes()`.

Instrument these `clean_up()` groups:

- early DDL/logging/cache prelude before `plugin_shutdown()`;
- `plugin_shutdown()`;
- `udf_free()`, `ha_end()`, transaction-log close, and XID-cache cleanup;
- `tdc_deinit()` and `mdl_destroy()`;
- cache/status/accounting/global object cleanup through `end_ssl()`;
- scheduler callback and scheduler pointer reset;
- `mysql_library_end()`;
- diagnostics, error, sysvar, and charset cleanup;
- final path/list/proxy cleanup.

The embedded performance probe resets/enables these counters together with the
existing embedded open/close perf counters and emits both detailed and compact
summary rows for ordinary and ownerless open/close samples.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, native storage, or WordPress
application behavior changes. The new functions are internal instrumentation
used by tests and probes, not public `libmylite` API.

## Directory And Lifecycle Impact

No directory layout change. `mylite_close()` still runs the same MariaDB
shutdown sequence and waits for the same cleanup work. The counters observe
cleanup time but do not keep any runtime state alive after close.

## Native Storage Impact

No native storage behavior changes. `ha_end()` and engine cleanup still run in
the existing MariaDB order.

## Build And Size Impact

The slice adds a small counter array, a header, and timing calls in shutdown
paths. It introduces no new dependency. Runtime overhead is paid only when the
internal performance probe enables the counters; disabled production shutdown
does only a few one-shot helper calls at shutdown boundaries.

## Test And Verification Plan

- Rebuild the production PHP embedded target so MariaDB-derived shutdown files
  are recompiled.
- Run a reduced production embedded performance probe and verify the new
  detailed and summary shutdown cleanup fields are emitted.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`.
- Run `git diff --check`.

## Verification Results

A reduced production probe on 2026-06-18 used
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
`MYLITE_PERF_SELECT_ITERATIONS=1`, and
`MYLITE_PERF_INSERT_ITERATIONS=1` against the `php-embedded-prod` build after
rebuilding the MariaDB embedded archive. The ordinary warm open/close sample
reported:

- `release_mysql_server_end_ms_avg=219.689`;
- `shutdown_server_end_embedded_server_ms_avg=219.584`;
- `shutdown_end_embedded_server_clean_up_ms_avg=219.581`;
- `shutdown_clean_up_plugin_shutdown_ms_avg=219.017`;
- all other reported `clean_up()` groups below `0.3 ms` each.

This proves the next optimization target is MariaDB `plugin_shutdown()`, not
client plugin cleanup, `my_end(0)`, `clean_up_mutexes()`, handler shutdown,
TDC/MDL teardown, library teardown, or charset/error cleanup in this sample.

## Acceptance Criteria

- The new MariaDB-side counter API compiles from both C and C++ shutdown files.
- Probe output retains the existing open-phase shutdown fields and adds
  shutdown cleanup fields with nonnegative values.
- The reduced production sample identifies which `mysql_server_end()` subgroup
  owns the dominant shutdown time.
- Documentation states this is attribution only, not a semantic optimization.

## Risks And Follow-Up

The subgrouping is deliberately coarse. If one broad `clean_up()` group still
dominates, the next slice should split that group further or remove a proven
unused cleanup path behind a MyLite build/profile guard. Any optimization that
skips MariaDB cleanup must separately prove repeated in-process open/close,
process-isolated PHP child behavior, native storage durability, and fork
maintenance impact.
