# Ordinary Ownerless Startup Performance Probe

## Problem

WordPress PHPUnit timing has repeatedly shown that full-suite wall time is a
noisy signal: build/setup, source-tree placement, filesystem sync latency, and
runner load can dominate the final number. The focused WordPress probes show
ordinary mysqli runtime parity with trunk, but they do not give a compact
per-process startup and core engine-cost signal that CI can print separately
from the full PHPUnit job.

MyLite needs a low-level embedded probe that reports ordinary and ownerless
open/close cost plus direct/prepared SQL throughput through the public C API.
The probe should be visible in CI logs, deterministic enough for comparisons,
and non-flaky by default.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:mylite_open()` delegates to
  `open_impl()`, which validates the path/configuration, calls
  `start_runtime()`, `connect_runtime()`, and system-table initialization.
- `packages/libmylite/src/database.cc:start_runtime()` performs fixed
  database-directory work for ordinary non-memory opens, including concurrency
  metadata creation, shared-memory preparation, runtime argument construction,
  optional ownerless hook setup, `mysql_server_init()`, and the checkpoint
  scheduler.
- `packages/libmylite/src/database.cc:connect_runtime()` calls
  `mysql_init()` and embedded `mysql_real_connect()`.
- `packages/libmylite/src/database.cc:mylite_exec()` dispatches ordinary
  direct SQL through `mysql_query()` and drains result sets through
  `store_and_emit_result()`.
- `packages/libmylite/src/database.cc:mylite_step()` dispatches ordinary
  prepared SQL through `mysql_stmt_execute()` and the MyLite result metadata
  path.
- `mariadb/libmysqld/libmysql.c:mysql_server_init()` initializes the embedded
  MariaDB server, `mysql_server_end()` shuts it down, `mysql_query()` wraps
  `mysql_real_query()`, and `mysql_stmt_execute()` dispatches prepared
  statements through the embedded statement method table.
- `mariadb/libmysqld/libmysqld.c:mysql_real_connect()` selects embedded
  connection methods when no external host is requested.

## Scope And Non-Goals

In scope:

- Add a first-party embedded executable,
  `mylite_embedded_performance_probe`, that prints parseable timing keys.
- Measure first ordinary create/open/close, warm ordinary open/close, warm
  ownerless first-probe open/close, cached warm ownerless open/close, ordinary
  direct `SELECT 1`, ordinary prepared `SELECT 1`, ordinary transactional
  prepared insert, and matching ownerless direct/prepared/insert probes.
- Use the public `libmylite` C API and `MYLITE_DURABILITY_FULL`, matching the
  durable directory shape used by correctness tests and application adapters.
- Emit compact summary lines for the main open/close subphases so production
  CI logs expose per-process startup/shutdown cost without requiring manual
  inspection of the detailed metric block.
- Keep performance assertions opt-in through environment thresholds so CI logs
  provide timing visibility without introducing load-sensitive failures.
- Run the probe as a separate embedded CI step after the build/test steps so
  its timing output is visible.

Out of scope:

- Failing CI on hard-coded timing thresholds.
- Replacing WordPress PHPUnit, ownerless stress, or external MariaDB replay
  evidence.
- Claiming full ownerless performance parity from a single local probe.

## Design

The probe creates a temporary `.mylite` database under `TMPDIR`, with a
separate MyLite runtime temp directory, and prints:

- iteration counts,
- temporary database path,
- ordinary cold create/open/close average,
- ordinary and ownerless warm open/close averages,
- ownerless first-probe open/close average before
  `concurrency/mylite-ownerless-platform.meta` exists,
- ordinary and ownerless direct `SELECT 1` rates,
- ordinary and ownerless prepared `SELECT 1` rates,
- ordinary and ownerless transactional prepared insert rates,
- ordinary and ownerless autocommit prepared insert rates.

Each ordinary warm open/close, ownerless first-probe open/close, ownerless
cached warm open/close, and active-runtime reconnect sample also emits compact
`mylite_perf_summary_*` subphase keys for:

- open total,
- platform probe,
- runtime start,
- runtime connect,
- system table checks,
- dictionary handoff,
- `mysql_server_init()`,
- close total,
- runtime release,
- ownerless reclaim,
- `mysql_server_end()` shutdown.

Those summary keys duplicate the most important information from the detailed
`*_open_phase_*` metric block and are intended for branch/main timing scans in
CI logs. They distinguish full process/runtime startup and shutdown from the
much cheaper active-runtime reconnect path. The ownerless first-probe sample
separates the one-time database-directory primitive proof from cached ownerless
warm open/close cost, so CI does not average one uncached proof into the
recurring ownerless startup number.

Default iteration counts are intentionally small enough for CI but large enough
to smooth timer noise:

- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS`, default `5`,
- `MYLITE_PERF_SELECT_ITERATIONS`, default `1000`,
- `MYLITE_PERF_INSERT_ITERATIONS`, default `200`.

`MYLITE_PERF_DURABILITY` can be set to `FULL`, `NORMAL`, or `OFF`; it defaults
to `FULL`.

Optional environment guardrails can be enabled by local runs or future CI
policy without changing the executable:

- `MYLITE_PERF_MAX_ORDINARY_WARM_OPEN_CLOSE_MS`,
- `MYLITE_PERF_MAX_OWNERLESS_FIRST_PROBE_OPEN_CLOSE_MS`,
- `MYLITE_PERF_MAX_OWNERLESS_WARM_OPEN_CLOSE_MS`,
- `MYLITE_PERF_MIN_ORDINARY_DIRECT_SELECT1_OPS`,
- `MYLITE_PERF_MIN_ORDINARY_PREPARED_SELECT1_OPS`,
- `MYLITE_PERF_MIN_ORDINARY_INSERT_TXN_OPS`,
- `MYLITE_PERF_MIN_ORDINARY_AUTOCOMMIT_INSERT_OPS`,
- `MYLITE_PERF_MIN_OWNERLESS_DIRECT_SELECT1_OPS`,
- `MYLITE_PERF_MIN_OWNERLESS_PREPARED_SELECT1_OPS`,
- `MYLITE_PERF_MIN_OWNERLESS_INSERT_TXN_OPS`,
- `MYLITE_PERF_MIN_OWNERLESS_AUTOCOMMIT_INSERT_OPS`.

## Compatibility Impact

No SQL semantics or public API behavior changes. The probe exercises ordinary
and ownerless public C API behavior that is already supported.

## Directory And Lifecycle Impact

The probe creates and removes a temporary MyLite-owned database directory. It
does not add new durable files or change the database layout.

## Native Storage Impact

No native storage format changes. The probe uses InnoDB tables with ordinary
primary keys and transactional prepared inserts.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The new executable is a test/probe target
only.

## CI Impact

The embedded CI job runs `mylite_embedded_performance_probe` as its own step
after the embedded CTest commands. The output appears in the job log with stable
`mylite_perf_*` keys so startup and engine timing can be compared without
scraping WordPress PHPUnit output.

## Test Plan

- Build `mylite_embedded_performance_probe` in `embedded-dev`.
- Run the probe with default iterations.
- Confirm the probe prints compact startup/shutdown subphase summary keys and
  separate ownerless first-probe versus cached warm open/close keys.
- Run the probe with reduced non-default iterations to validate environment
  controls.
- Run the embedded lifecycle/open-close test and ownerless hook tests to make
  sure the probe target did not disturb existing lifecycle coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The probe builds in embedded presets.
- The probe prints parseable open/close and SQL throughput keys.
- The probe prints compact startup/shutdown subphase summary keys for ordinary
  warm open/close, ownerless first-probe open/close, ownerless cached warm
  open/close, and active-runtime reconnect samples.
- The probe exits successfully without thresholds under ordinary CI load.
- Optional threshold environment variables can fail the probe when limits are
  missed.
- CI has a separate visible performance-probe step.

## Risks And Follow-Up

- Timing remains host-sensitive. Treat this as trend evidence, not a universal
  benchmark.
- A future slice can add internal phase timing around `start_runtime()` if the
  black-box open/close signal regresses.
- Future CI can add soft artifact collection or threshold policy after enough
  baseline samples exist.
