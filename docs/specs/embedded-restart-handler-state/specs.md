# Embedded Restart Handler State

## Problem

MyLite opens and closes the MariaDB embedded runtime behind `mylite_open_v2()`
and `mylite_close()`. Existing tests cover a small number of repeated
open/close cycles, but the storage-engine smoke binary is now close to MariaDB
process-global handler and plugin restart limits. Adding one more full embedded
startup exposed `Too many plugins loaded. Limit is 64`, and a full
storage-smoke rerun also produced one transient embedded comparison segfault
that disappeared when rerun.

The next roadmap work will add more lifecycle-heavy catalog and compatibility
coverage. MyLite needs deterministic repeated embedded init/end behavior in one
process before more coverage is stacked on top.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/lib_sql.cc:init_embedded_server()` starts MariaDB embedded
  runtime state and `end_embedded_server()` calls `clean_up()` plus
  `clean_up_mutexes()` when `mysql_server_end()` runs.
- `mariadb/sql/mysqld.cc:clean_up()` calls `plugin_shutdown()` before
  `ha_end()`.
- `mariadb/sql/sql_plugin.cc:plugin_shutdown()` deinitializes and deletes
  plugin records, then clears plugin arrays and hashes.
- `mariadb/sql/handler.cc` keeps process-global handler state in
  `hton2plugin`, `installed_htons`, `total_ha`, `total_ha_2pc`,
  `savepoint_alloc_size`, and `ddl_recovery_done`.
- `mariadb/sql/handler.cc:setup_transaction_participant()` reuses holes in
  `hton2plugin` but keeps `total_ha` as the high-water mark. If handler slots
  are not fully reusable across embedded restarts, startup eventually reports
  `Too many plugins loaded. Limit is 64`.
- `mariadb/sql/handler.cc:ha_init()` appends to `savepoint_alloc_size` on each
  startup, and `ha_signal_ddl_recovery_done()` leaves `ddl_recovery_done` true
  after startup. Neither value is reset in upstream `ha_end()` because normal
  server shutdown exits the process.
- MyLite already carries two narrow embedded-restart patches in
  `mariadb/sql/mysqld.cc` and `mariadb/sql/sql_locale.cc` for scheduler and
  error-message state. Handler state is the remaining restart-sensitive area
  found by current tests.
- `packages/libmylite/src/database.cc:start_runtime()` and
  `release_runtime()` call `mysql_server_init()` / `mysql_server_end()` once
  per file-owned runtime lifetime.

## Design

Add an embedded-only handler cleanup step in `ha_end()` after plugin shutdown
has run. The cleanup should reset only process-global handler state that is
recreated during the next embedded startup:

- clear `hton2plugin`,
- clear `installed_htons`,
- set `total_ha` and `total_ha_2pc` back to `0`,
- reset debug-only `failed_ha_2pc`,
- set `savepoint_alloc_size` back to `0`, and
- set `ddl_recovery_done` back to `false`.

Keep the patch under `#ifdef EMBEDDED_LIBRARY`. A normal server process exits
after shutdown, so the fork delta should stay scoped to the embedded restart
case MyLite actually exercises.

## Supported Scope

- Repeated `mylite_open_v2()` / `mylite_close()` cycles for the same file in one
  process.
- The storage-smoke build where the static MyLite handler is registered with
  the MariaDB embedded archive.
- Existing two-handle shared-runtime behavior, where the MariaDB runtime should
  stay alive until the final handle closes.

## Non-Goals

- Making MariaDB embedded runtime startup cheap.
- Supporting simultaneous independent MariaDB embedded runtimes for different
  files in one process.
- Fixing every process-global MariaDB variable that is harmless in current
  MyLite startup/shutdown coverage.
- Changing plugin initialization order or disabling additional plugins.

## Compatibility Impact

This does not change SQL compatibility. It strengthens the embedded lifecycle
contract that repeated MyLite open/close cycles in one process should not
consume process-global MariaDB handler slots or corrupt later embedded startup.

## DDL Metadata Routing Impact

No DDL metadata behavior changes. The slice exists so future DDL coverage can
add reopen-heavy cases without hitting unrelated embedded restart limits.

## Single-File And Embedded-Lifecycle Impact

No file-format or durable sidecar change is introduced. Runtime directories
should still be removed after final close, and shared runtime reference counting
should remain unchanged.

## Public API And File-Format Impact

The public C API and `.mylite` file format do not change.

## Storage-Engine Routing Impact

Static MyLite handler registration should remain available after repeated
embedded restarts in the storage-smoke profile.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol or integration-package changes are included.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact should be negligible: the change is
an embedded-only reset helper in MariaDB handler shutdown.

## Test And Verification Plan

- Increase the repeated open/close lifecycle test so it exercises enough
  same-process embedded restarts to exceed the previous fragile threshold.
- Run that lifecycle test under both `embedded-dev` and `storage-smoke-dev`.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, and
  the embedded-lifecycle compatibility harness group.
- Run the storage-smoke suite at least once after the fix to catch lifecycle
  order regressions across adjacent embedded test binaries.

## Acceptance Criteria

- Repeated open/close coverage passes with more than two embedded restarts in
  one process.
- The storage-smoke build no longer reports static plugin slot exhaustion in
  repeated MyLite restart coverage.
- Existing shared-runtime, busy-open, no-defaults, and cleanup tests still pass.
- Docs record the new embedded handler-state restart reset.

## Implementation Status

Implemented by resetting the embedded handler registry state in
`mariadb/sql/handler.cc:ha_end()` after plugin shutdown. The public
open/close lifecycle test now runs 40 same-process `mylite_open_v2()` /
`mylite_close()` cycles, which exceeds the previously failing handler-slot
threshold in the storage-smoke profile.

## Risks And Unresolved Questions

- MariaDB has many process-global variables that were not designed for repeated
  init/end. This slice resets the handler state implicated by current failures;
  future tests may expose additional restart-sensitive globals.
- The transient embedded comparison segfault was not deterministic. Passing
  reruns reduce suspicion, but the compatibility harness should continue to
  exercise `sql-comparison` as lifecycle coverage expands.
