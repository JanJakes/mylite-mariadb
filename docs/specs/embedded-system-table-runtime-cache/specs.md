# Embedded System-Table Runtime Cache

## Problem

The production active-runtime reconnect probe shows that a second same-process
handle already skips `mysql_server_init()` and `mysql_server_end()` while an
anchor handle keeps the embedded runtime alive. The remaining reconnect cost
still includes about 1 ms of repeated `mysql.*` compatibility-table DDL per
handle.

Those statements are idempotent and are needed once after a persistent
read/write runtime starts. Re-running them for every later handle in the same
active runtime adds cost to WordPress/PHP same-process reconnect paths without
improving durability or cross-process coordination.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:start_runtime()` increments
  `g_runtime.ref_count` and returns early when a matching runtime is already
  active for the same database directory, access mode, ownerless mode, and
  durability policy.
- `packages/libmylite/src/database.cc:ensure_core_system_tables()` currently
  runs `execute_core_system_table_statements()` after every persistent
  read/write handle connects, protected by an in-process mutex and the
  directory-owned `SYSTEM_TABLES` byte-range lock.
- `packages/libmylite/src/database.cc:execute_core_system_table_statements()`
  executes MyLite's `CREATE DATABASE IF NOT EXISTS mysql`,
  `CREATE TABLE IF NOT EXISTS mysql.proc`, and
  `CREATE TABLE IF NOT EXISTS mysql.procs_priv` bootstrap statements.
- MariaDB source keeps `IF NOT EXISTS` as explicit DDL syntax and behavior:
  `mariadb/sql/structs.h:OPT_IF_NOT_EXISTS`,
  `mariadb/sql/sql_yacc.yy:CREATE DATABASE IF NOT EXISTS` and
  `CREATE TABLE IF NOT EXISTS` grammar paths, and
  `mariadb/sql/sql_parse.cc` / `mariadb/sql/sql_table.cc` comments around
  repeated `CREATE TABLE IF NOT EXISTS` handling.
- `packages/libmylite/src/database.cc:release_runtime()` only performs full
  MariaDB embedded shutdown when the runtime reference count drops to zero, and
  `clear_runtime_state()` resets process-global runtime metadata for the next
  runtime.

## Scope And Non-Goals

In scope:

- Remember that core system-table bootstrap succeeded for the current active
  persistent read/write runtime.
- Skip the repeated bootstrap DDL for later same-process handles on that same
  runtime.
- Reset the readiness flag when the final runtime closes or startup fails.
- Keep the existing cross-process bootstrap byte-range lock for the first
  handle in each process runtime.
- Expose an exact embedded-open performance counter for system-table bootstrap
  executions so tests and probes do not infer skips from timing resolution.
- Add a WordPress mysqli active-runtime reconnect perf-probe metric that keeps
  one anchor link open while timing second-link connect/close loops.
- Add focused open/close coverage and update performance documentation.

Out of scope:

- Changing `mylite_close()` semantics.
- Holding a hidden runtime after the last handle closes.
- Skipping system-table bootstrap across runtime restarts or across processes.
- Changing memory-database bootstrap behavior.
- Changing ownerless recovery, locks, redo, page-version WAL, or PHP adapter
  lifetime rules.

## Design

Add a `core_system_tables_ready` flag to `RuntimeState`.

For persistent read/write opens, `ensure_core_system_tables()` will:

1. Return immediately for read-only opens, preserving the current behavior.
2. Return immediately when the active runtime has already completed
   system-table bootstrap.
3. Otherwise take the existing in-process `g_system_table_mutex`.
4. Re-check the flag while holding the mutex so concurrent same-process opens
   cannot race ahead of the first bootstrap.
5. Acquire the existing directory-owned system-table byte-range lock.
6. Execute the current bootstrap statements.
7. Mark `core_system_tables_ready` only after the statements succeed.

The flag is process-local and runtime-local. A separate process still performs
its first bootstrap under the shared byte-range lock. A later runtime in the
same process starts from `false` after `clear_runtime_state()`.

## Compatibility Impact

No SQL behavior changes. The same idempotent bootstrap statements still run for
the first persistent read/write handle in each process runtime. Later handles
avoid duplicate bootstrap work only while the same runtime remains active.

## Database Directory And Lifecycle Impact

No directory-layout change. Durable state remains the existing MariaDB-native
`mysql.*` compatibility tables inside the MyLite database directory. The
existing cross-process lock remains the coordination boundary for first
bootstrap work in each process runtime.

## Native Storage Impact

No native storage format or recovery change. The optimization removes repeated
successful DDL execution from same-process reconnects; it does not bypass
first-runtime MariaDB DDL, file creation, or recovery.

## Build, Size, And Dependency Impact

No new dependency. The binary-size impact is one runtime boolean plus a small
branch in the open path and one internal performance counter. The WordPress
probe emits two extra diagnostic keys.

## Test And Verification Plan

- Extend `mylite_embedded_open_close_test baseline` so a second handle on an
  active persistent runtime records one `ensure_core_system_tables()` call but
  zero system-table bootstrap executions, then prove a new runtime after final
  close executes bootstrap once.
- Build `mylite_embedded_open_close_test` and
  `mylite_embedded_performance_probe` in `php-embedded-prod`.
- Run the focused embedded open/close CTest.
- Run a reduced production performance probe and confirm active-runtime
  reconnect emits zero system-table statement time.
- Rebuild the WordPress PHP extension production build and run the WordPress
  `perf-probe` phase to confirm the new mysqli active-runtime reconnect metric
  is emitted.
- Run adjacent embedded lifecycle/ownerless tests that exercise open/close and
  ownerless hook binding.
- Run production build/test, format/tidy, and `git diff --check`.

## Verification Results

Local production verification on 2026-06-08:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_open_close_test mylite_embedded_performance_probe`: passed.
- `ctest --preset php-embedded-prod -R libmylite.embedded-open-close
  --output-on-failure`: passed.
- A reduced production probe with `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=3`,
  `MYLITE_PERF_SELECT_ITERATIONS=5`, and `MYLITE_PERF_INSERT_ITERATIONS=2`
  passed. Ordinary active-runtime reconnect reported
  `mylite_perf_ordinary_active_runtime_reconnect_ms_avg=0.778` with
  `system_table_calls=3` and `system_table_executions=0`; ownerless
  active-runtime reconnect reported
  `mylite_perf_ownerless_active_runtime_reconnect_ms_avg=1.079` with
  `system_table_calls=3` and `system_table_executions=0`.
- Focused adjacent production embedded tests passed:
  `libmylite.embedded-ownerless-mdl-hooks`,
  `libmylite.embedded-ownerless-trx-hooks`,
  `libmylite.embedded-ownerless-innodb-lock-hooks`,
  `libmylite.ownerless-primitives`,
  `libmylite.embedded-ownerless-directory-lifecycle`,
  `libmylite.embedded-ownerless-product-hooks`,
  `libmylite.embedded-storage`, and
  `libmylite.embedded-transactions-recovery`.
- The default production probe passed. Fresh warm open/close still reported
  one bootstrap execution per fresh runtime:
  ordinary `system_table_calls=5` and `system_table_executions=5`, ownerless
  `system_table_calls=5` and `system_table_executions=5`. Active-runtime
  reconnect reported zero bootstrap executions while keeping MariaDB startup
  and shutdown skipped: ordinary active-runtime reconnect averaged `0.827 ms`
  with `system_table_calls=5` and `system_table_executions=0`; ownerless
  active-runtime reconnect averaged `2.355 ms` with `system_table_calls=5`
  and `system_table_executions=0`.
- `MYLITE_WORDPRESS_PHASE=build-php` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod` and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` passed after rebuilding the
  WordPress PHP extension production build.
- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `MYLITE_WORDPRESS_PHASE=perf-probe` with the same production build directory
  and CI-sized iteration counts passed after adding the anchored mysqli metric.
  The run reported stock PHP startup `71.850 ms`, PHP wrapper startup
  `125.943 ms`, process plus connect/close `591.173 ms`, process/connect
  delta `465.230 ms`, full in-process mysqli connect/close `472.590 ms`,
  anchored active-runtime reconnect `3.647 ms`, `SELECT 1` `247.53 ops/s`,
  transactional inserts `345.15 ops/s`, point selects `221.61 ops/s`,
  prepared autocommit inserts `363.66 ops/s`, and direct autocommit inserts
  `706.92 ops/s`.
- `cmake --build --preset php-embedded-prod`: passed.
- `cmake --build --preset prod`: passed.
- `ctest --preset prod --output-on-failure`: passed `24/24`.
- `cmake --build --preset format-check-prod`: passed.
- `cmake --build --preset tidy-prod`: passed.
- `git diff --check`: passed.

## Acceptance Criteria

- The first persistent read/write handle still initializes core system tables.
- A second same-process handle on the same active runtime skips the bootstrap
  statements.
- The flag resets after final close.
- Read-only and memory paths keep their existing behavior.
- Production active-runtime reconnect timings no longer include repeated
  system-table statement work and report zero bootstrap executions.

## Risks And Follow-Up

- This only improves same-process reconnects while a runtime is already active.
  PHPUnit process-isolated child startup still pays full MariaDB embedded
  runtime startup and shutdown unless a separate explicit lifecycle design is
  accepted.
- Timing remains host-sensitive; use the production probe counters rather than
  hard thresholds for CI pass/fail.
