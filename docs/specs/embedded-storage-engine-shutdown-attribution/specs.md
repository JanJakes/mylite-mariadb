# Embedded Storage Engine Shutdown Attribution

## Problem

The embedded plugin-shutdown attribution slice narrowed ordinary warm
open/close shutdown cost to storage-engine plugin deinitialization during
MariaDB `reap_plugins()`: a reduced production sample reported `223.800 ms`
across 9 storage-engine deinit calls. That still does not identify which engine
or which generic handlerton finalization phase owns the cost.

This slice adds attribution only. It does not skip `ha_finalize_handlerton()`,
retain storage engines across `mylite_close()`, trim the embedded engine set, or
change InnoDB/Aria/MyISAM/CSV/MEMORY lifecycle semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_plugin.cc` maps storage-engine plugin deinitialization
  through `plugin_type_deinitialize[MYSQL_STORAGE_ENGINE_PLUGIN]`, which points
  to `ha_finalize_handlerton()`.
- `mariadb/sql/sql_plugin.cc` `plugin_deinitialize()` removes status variables,
  calls the type-level deinitializer or plugin deinit callback, marks the plugin
  uninitialized on success, optionally logs ref-count errors, and deinitializes
  plugin variables.
- `mariadb/sql/handler.cc` `ha_finalize_handlerton()` receives the
  `st_plugin_int`, reads `plugin->data` as a `handlerton`, clears the installed
  handlerton slot, calls `hton->panic(HA_PANIC_CLOSE)` when present, calls the
  storage-engine plugin's own `deinit` callback when present, frees table
  option sysvars, updates discovery counters, clears the `hton2plugin` slot, and
  frees the handlerton.
- `mariadb/storage/innobase/handler/ha_innodb.cc` registers plugin name
  `InnoDB` and sets `innobase_hton->panic = innobase_end`; `innobase_end()` runs
  `innodb_shutdown()` and destroys `log_requests.mutex` when InnoDB was started.
- Representative built-in storage-engine plugin names registered in the
  embedded tree include `InnoDB`, `Aria`, `MyISAM`, `MEMORY`, `CSV`,
  `partition`, `SQL_SEQUENCE`, `SEQUENCE`, `MRG_MyISAM`, and
  `PERFORMANCE_SCHEMA`.

## Design

Extend the internal `mylite_embedded_shutdown_perf` counter family with
storage-engine finalization counters:

- call count and total time for `ha_finalize_handlerton()`;
- null-handlerton calls;
- installed-handlerton unregister time;
- `hton->panic(HA_PANIC_CLOSE)` time;
- plugin-specific `deinit` callback time;
- table option sysvar cleanup time;
- discovery-counter cleanup time;
- `hton2plugin` slot-clear time;
- handlerton free time;
- total time by fixed storage-engine plugin name for the representative
  built-in engines, plus an `other` bucket.

The embedded performance probe emits detailed
`shutdown_phase_storage_engine_*` rows and compact summary rows for the
dominant expected buckets: total finalization, `panic`, plugin callback deinit,
and InnoDB/Aria/MyISAM/CSV/MEMORY per-engine totals. The implementation keeps
the classification static and shutdown-only so normal SQL execution and
stats-disabled production paths remain unaffected.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, native storage, plugin, or
WordPress application behavior changes. The new counters are internal
instrumentation used by the performance probe.

## Directory And Lifecycle Impact

No directory layout change. `mylite_close()` still deinitializes storage engines
through MariaDB's existing `ha_finalize_handlerton()` path and still frees each
handlerton before close returns.

## Native Storage Impact

No native storage behavior changes. InnoDB, Aria, MyISAM, CSV, MEMORY, and
other built-in engine finalizers still run in MariaDB's existing order.

## Build And Size Impact

The slice adds shutdown-only counters and fixed-name classification. It
introduces no new dependency and does not change the embedded plugin profile.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build the production embedded performance probe.
- Run a reduced production embedded performance probe and verify storage-engine
  finalization phase and per-engine rows are emitted.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`.
- Run `git diff --check`.

## Verification Results

A reduced production probe with one open/close iteration reported:

- `storage_engine_finalize_calls=9`;
- `storage_engine_finalize_total_ms=216.159`;
- `storage_engine_finalize_panic_ms=216.142`;
- `storage_engine_finalize_plugin_deinit_ms=0.002`;
- `storage_engine_finalize_innodb_calls=1`;
- `storage_engine_finalize_innodb_ms=215.685`;
- `storage_engine_finalize_aria_calls=1`;
- `storage_engine_finalize_aria_ms=0.458`;
- all other fixed engine buckets were below `0.006 ms`, with no
  `performance_schema` or `other` calls.

The dominant ordinary warm open/close shutdown cost therefore belongs to InnoDB
handlerton panic shutdown, not generic handlerton cleanup or other embedded
engines. The next performance slice should instrument `innodb_shutdown()`
subphases before considering lifecycle retention or skipping shutdown work.

## Acceptance Criteria

- `handler.cc` compiles with the internal shutdown counter header.
- Probe output identifies which storage-engine plugin owns the dominant
  ordinary warm open/close shutdown time.
- Probe output identifies whether the dominant handlerton phase is `panic`,
  plugin callback `deinit`, or generic cleanup.
- Documentation records that this is attribution only and does not claim an
  engine-shutdown optimization.

## Risks And Follow-Up

If InnoDB `hton->panic` dominates, the next slice should instrument
`innodb_shutdown()` subphases before considering any lifecycle cache or
retained-engine mode. Retaining initialized storage-engine state across
`mylite_close()` would be a semantic change and needs separate durability,
repeated-open, PHP child, and fork-maintenance evidence.
