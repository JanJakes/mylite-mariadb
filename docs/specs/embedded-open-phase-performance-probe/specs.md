# Embedded Open Phase Performance Probe

## Problem

The production WordPress perf probe shows PHP process startup is not the
dominant cost. Current ownerless head on 2026-06-08 reported stock PHP startup
`60.313 ms`, MyLite-extension PHP startup `82.894 ms`, process plus MyLite
connect/close `627.722 ms`, and in-process mysqli connect/close `446.075 ms`.

The existing embedded performance probe reports ordinary and ownerless
open/close averages, but it treats `mylite_open()` and `mylite_close()` as
black boxes. That makes the next optimization ambiguous: the cost may be
MariaDB embedded startup, filesystem layout work, fixed ownerless coordination
metadata, core `mysql.*` compatibility bootstrap, connection creation, or
close-side shutdown/reclaim.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens WordPress
  mysqli connections with `mylite_open(path, ..., MYLITE_OPEN_READWRITE |
  MYLITE_OPEN_CREATE, NULL)`.
- `packages/libmylite/src/database.cc:open_impl()` validates arguments,
  normalizes the directory, prepares the database directory, starts the
  embedded runtime, connects an embedded `MYSQL`, ensures core `mysql.*`
  compatibility tables, and initializes ownerless dictionary generation.
- `packages/libmylite/src/database.cc:start_runtime()` includes fixed
  directory-owned coordination setup, runtime layout creation, concurrency
  shared-memory mapping, page-log/checkpoint file setup, optional ownerless
  hook/recovery work, `mysql_server_init()`, and checkpoint-scheduler startup.
- `packages/libmylite/src/database.cc:connect_runtime()` calls `mysql_init()`
  and embedded `mysql_real_connect()`.
- `packages/libmylite/src/database.cc:mylite_close()` rolls back active
  transactions, closes the embedded connection, and calls `release_runtime()`.
- `packages/libmylite/src/database.cc:release_runtime()` stops ownerless
  scheduler state, performs close-time reclaim/redo handling, resets native
  hooks, calls `mysql_thread_end()` and `mysql_server_end()`, unmaps
  concurrency state, and removes runtime directories.
- `mariadb/libmysqld/libmysql.c:mysql_server_init()` initializes the MariaDB
  client and embedded server environment, and `mysql_server_end()` tears it
  down.
- `mariadb/libmysqld/libmysqld.c:mysql_real_connect()` selects embedded
  connection methods when no external host is requested and creates the
  embedded THD/connection state.
- `mariadb/sql/handler.cc:ha_end()` resets embedded handler state so a later
  `mysql_server_init()` in the same process can rebuild storage-engine state.

## Scope And Non-Goals

In scope:

- Add opt-in internal timing counters for `mylite_open()`, `start_runtime()`,
  `connect_runtime()`, `ensure_core_system_tables()`, `mylite_close()`, and
  `release_runtime()`.
- Expose the counters through test/probe-only internal C symbols used by
  `mylite_embedded_performance_probe`.
- Emit parseable `mylite_perf_*_open_phase_*` timing keys from the existing
  embedded performance probe for ordinary and ownerless warm open/close loops.
- Keep counters disabled by default and avoid public `mylite.h` API changes.

Out of scope:

- Changing MariaDB startup semantics.
- Adding hard CI timing thresholds.
- Optimizing a phase before the new probe identifies the dominant local cost.
- Adding PHP extension APIs or persistent PHP connections.

## Design

Add a small internal atomic counter table in `database.cc`, enabled only when
the probe calls:

- `mylite_embedded_open_perf_set_enabled(int)`;
- `mylite_embedded_open_perf_reset(void)`;
- `mylite_embedded_open_perf_read(uint64_t *out_values, size_t value_count)`.

The counter table records call counts and elapsed nanoseconds for:

- top-level open stages: validation, allocation/path normalization, runtime
  path validation, directory preparation, ownerless platform probe, startup
  lock, `start_runtime()`, `connect_runtime()`, system tables, and dictionary
  generation;
- runtime startup stages: database lock, concurrency metadata, shared-memory
  preparation, runtime layout/arguments, shared-memory mapping, page-log and
  checkpoint open, hook/recovery setup, bootstrap lock, `mysql_server_init()`,
  post-start hooks, redo backup update, and scheduler startup;
- connection stages: `mysql_init()` and `mysql_real_connect()`;
- system-table stages: serialization lock and DDL statements;
- close stages: rollback, connection close, and runtime release;
- final runtime-release stages: scheduler stop, startup lock, reclaim, redo
  capture, hook reset, MariaDB shutdown, redo restore, shared-memory unmap,
  runtime-directory cleanup, and database-lock release.

`mylite_embedded_performance_probe` enables and resets the table around the
ordinary and ownerless warm open/close loops, then prints totals and average
milliseconds per relevant call count. The probe keeps the existing black-box
open/close keys so previous CI trends stay comparable.

## Compatibility Impact

No SQL, mysqli, PHP, or public C API behavior changes. The new symbols are not
declared in `mylite.h` and are used only by the embedded test/probe target.

## Database Directory And Lifecycle Impact

No directory-layout change. The probe observes the existing open and close
lifecycle; it does not add durable files or alter cleanup policy.

## Native Storage Impact

No native storage format change. The counters observe MariaDB embedded startup
and shutdown, including InnoDB startup as driven by existing MyLite arguments.

## Build, Size, And Dependency Impact

No new dependencies. Production `libmylite` gains a small disabled-by-default
counter table and timing checks in open/close paths only.

## Initial Results

A default production `mylite_embedded_performance_probe` run under
`php-embedded-prod` on 2026-06-08 reported:

- ordinary warm open/close `345.520 ms` average;
- ordinary open total `113.802 ms` average, including `start_runtime()`
  `112.102 ms`, `mysql_server_init()` `111.525 ms`, embedded connect
  `0.495 ms`, and system tables `1.095 ms`;
- ordinary close total `231.715 ms` average, including runtime release
  `231.281 ms` and `mysql_thread_end()`/`mysql_server_end()` `230.066 ms`;
- ownerless warm open/close `407.799 ms` average;
- ownerless open total `159.079 ms` average, including ownerless platform
  probe `9.416 ms`, `start_runtime()` `147.670 ms`, `mysql_server_init()`
  `146.013 ms`, embedded connect `0.686 ms`, and system tables `1.039 ms`;
- ownerless close total `248.717 ms` average, including runtime release
  `247.935 ms`, scheduler stop `2.326 ms`, and
  `mysql_thread_end()`/`mysql_server_end()` `243.282 ms`.

The current local bottleneck is therefore MariaDB embedded runtime startup and
shutdown. Connection creation and MyLite system-table bootstrap are not the
dominant WordPress connect/open cost.

## Test Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the probe with reduced iterations and confirm ordinary and ownerless
  `open_phase_*` keys are emitted.
- Run the production probe with CI-like iterations to record current phase
  attribution.
- Run the focused embedded open/close and ownerless hook/primitive tests.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local production verification on 2026-06-08:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`: passed.
- Reduced `mylite_embedded_performance_probe` run emitted the new
  `open_phase_*` keys for ordinary and ownerless open/close.
- Default `mylite_embedded_performance_probe`: passed with the attribution
  recorded above.
- `MYLITE_WORDPRESS_PHASE=build-php` against
  `build/wordpress-php-embedded-prod`: passed after rebuilding the production
  PHP extension.
- Production WordPress `perf-probe` after the rebuild reported stock PHP
  startup `52.812 ms`, MyLite-extension PHP startup `78.441 ms`,
  process-plus-connect `576.489 ms`, connect delta `498.048 ms`, in-process
  connect/close `445.340 ms`, `SELECT 1` `265.15 ops/s`, transactional
  inserts `377.42 ops/s`, point selects `228.83 ops/s`, prepared autocommit
  inserts `370.51 ops/s`, and direct autocommit inserts `749.34 ops/s`.
- Focused embedded tests passed:
  `libmylite.embedded-ownerless-innodb-lock-hooks`,
  `libmylite.ownerless-primitives`, and `libmylite.embedded-open-close`.
- `cmake --build --preset prod`: passed.
- `ctest --preset prod --output-on-failure`: passed `24/24`.
- `cmake --build --preset format-check-prod`: passed after formatting the
  touched source files.
- `cmake --build --preset tidy-prod`: passed.
- `git diff --check`: passed.

## Acceptance Criteria

- The embedded performance probe still prints the existing black-box timing
  keys.
- The probe prints parseable ordinary and ownerless open-phase keys with
  nonzero `open_calls`, `start_runtime_calls`, `connect_calls`, `close_calls`,
  and `release_runtime_calls`.
- The counters remain disabled unless the probe enables them.
- No public header or compatibility matrix update is required because behavior
  and supported surfaces do not change.

## Risks And Follow-Up

- Timing remains host-sensitive. Use the phase split to identify the local
  dominant cost before optimizing.
- The counters add tiny disabled-path checks to open/close. They intentionally
  do not instrument per-statement hot paths.
- Follow-up optimization candidates depend on the resulting attribution:
  MariaDB startup policy, fixed coordination-file setup, system-table
  bootstrap, or close-side shutdown/reclaim.
