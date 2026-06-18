# Embedded Plugin Shutdown Attribution

## Problem

The embedded shutdown cleanup attribution slice narrowed ordinary warm
open/close shutdown cost to MariaDB `plugin_shutdown()`: a reduced production
sample reported `219.017 ms` in `plugin_shutdown()` out of `219.581 ms` in
`clean_up()`. `plugin_shutdown()` is still a broad subsystem boundary. It may
represent storage-engine finalization, information-schema plugin cleanup,
daemon/audit/auth/plugin metadata teardown, dynamic plugin memory disposal, or
reference reaping.

This slice adds attribution only. It does not skip plugin shutdown, alter plugin
dependency ordering, keep plugins initialized after `mylite_close()`, or change
which plugins are built into the embedded profile.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/mysqld.cc` `clean_up()` calls `plugin_shutdown()` before
  `udf_free()`, `ha_end()`, transaction-log close, TDC teardown, MDL teardown,
  MariaDB library cleanup, and charset/error cleanup.
- `mariadb/sql/sql_plugin.cc` `plugin_shutdown()` frees auto GTID engine-list
  state, locks `LOCK_plugin`, repeatedly calls `reap_plugins()` while marking
  ready plugins deleted, copies remaining plugin pointers, unlocks
  `LOCK_plugin`, calls `plugin_deinitialize()` for each plugin that is not
  already uninitialized/freed/disabled, relocks to check refcounts and
  `plugin_del()` each dying plugin, cleans global and max system variables,
  destroys `LOCK_plugin`, then frees plugin hashes, plugin arrays, dynamic
  plugin memory, bookmark hash, and plugin memory roots.
- `mariadb/sql/sql_plugin.cc` `plugin_deinitialize()` chooses either a
  type-level deinitializer from `plugin_type_deinitialize[]` or the plugin's own
  `deinit` callback, calls it, marks the plugin uninitialized on success, logs
  ref-count errors when requested, and deinitializes plugin variables.
- Plugin type ids are defined in `mariadb/include/mysql/plugin.h`:
  UDF, storage engine, full-text parser, daemon, information schema, audit,
  replication, authentication, password validation, encryption, data type, and
  function.

## Design

Extend the internal `mylite_embedded_shutdown_perf` counter family with
`plugin_shutdown()` phase counters:

- call count and total time;
- GTID auto-plugin list free;
- lock/reap/mark loop;
- reaped plugin deinitialization inside `reap_plugins()`;
- reaped plugin deletion inside `reap_plugins()`;
- forced-deinit preparation;
- plugin deinitialize loop;
- refcount check and `plugin_del()` loop;
- global/max system variable cleanup;
- `LOCK_plugin` destroy;
- plugin hash/dynamic-array disposal;
- dynamic plugin memory free;
- bookmark and plugin memory-root free.

Within the reaped and forced deinitialize loops, count and time
`plugin_deinitialize()` by MariaDB plugin type. The first performance question
is whether the dominant cost belongs to storage-engine plugin finalization or to
another plugin class; this slice does not yet split inside a storage-engine
finalizer such as InnoDB.

The embedded performance probe emits detailed `shutdown_phase_plugin_*` rows and
compact `shutdown_plugin_*_ms_avg` summary rows for the ordinary and ownerless
open/close lifecycle samples.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, native storage, plugin, or
WordPress application behavior changes. The new counters are internal
instrumentation used by the performance probe.

## Directory And Lifecycle Impact

No directory layout change. `mylite_close()` still runs `plugin_shutdown()` in
MariaDB's existing order, and plugin state is not retained after close.

## Native Storage Impact

No native storage behavior changes. Storage-engine plugin deinitializers still
run through MariaDB's existing plugin shutdown path.

## Build And Size Impact

The slice adds counters and timing calls in shutdown-only code. It introduces no
new dependency. Disabled production shutdown only pays the existing helper
checks at shutdown boundaries.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build the production embedded performance probe.
- Run a reduced production embedded performance probe and verify
  `plugin_shutdown()` phase and plugin-type rows are emitted.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`.
- Run `git diff --check`.

## Verification Results

A reduced production probe with one open/close iteration reported:

- `plugin_shutdown_total_ms_avg=223.868`;
- `plugin_shutdown_reap_loop_ms_avg=223.846`;
- `plugin_shutdown_reap_deinitialize_ms_avg=223.822`;
- `plugin_shutdown_deinitialize_ms_avg=0.000`;
- `plugin_deinit_storage_engine_calls=9`;
- `plugin_deinit_storage_engine_ms=223.800`;
- `plugin_deinit_information_schema_calls=31`;
- `plugin_deinit_information_schema_ms=0.008`.

The dominant ordinary warm open/close shutdown cost therefore belongs to
storage-engine plugin deinitialization during the `reap_plugins()` path. The
next performance slice should split that storage-engine time by engine/plugin
name or instrument the dominant engine finalizer directly before changing close
semantics.

## Acceptance Criteria

- `sql_plugin.cc` compiles with the internal shutdown counter header.
- Probe output identifies which plugin-shutdown phase and plugin type owns the
  dominant ordinary warm open/close shutdown time.
- Documentation records that this is attribution only and does not claim a
  plugin-shutdown optimization.

## Risks And Follow-Up

If storage-engine plugin deinitialization dominates, the next slice should split
storage-engine plugin shutdown by engine/plugin name or instrument the specific
engine finalizer before considering any lifecycle retention or plugin-profile
trim. Retaining initialized engine/plugin state across `mylite_close()` would be
a semantic change and needs separate durability, repeated-open, PHP child, and
fork-maintenance evidence.
