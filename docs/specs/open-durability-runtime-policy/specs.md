# Open Durability Runtime Policy

## Problem

The WordPress and embedded performance probes show that durable MyLite startup
and write-heavy test phases pay visible InnoDB sync cost. The public
`mylite_open_config` already exposes a `durability` field and validates
`MYLITE_DURABILITY_FULL`, `MYLITE_DURABILITY_NORMAL`, and
`MYLITE_DURABILITY_OFF`, but runtime startup still hard-coded
`--innodb-flush-log-at-trx-commit=1`. Callers therefore could not measure or
choose the documented durability/performance tradeoff at open time.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:validate_open_args()` already accepts the
  three MyLite durability constants when the `durability` field is present.
- `packages/libmylite/src/database.cc:runtime_arguments()` always supplied
  `--innodb-flush-log-at-trx-commit=1`, so the validated config field was not
  reflected in the embedded MariaDB runtime.
- MariaDB source maps `innodb_flush_log_at_trx_commit` in
  `mariadb/storage/innobase/handler/ha_innodb.cc`; value `1` flushes at each
  commit, value `2` writes at commit and flushes on the periodic timeout, and
  value `0` defers both writes and flushes to the timeout. InnoDB commit code
  in `mariadb/storage/innobase/trx/trx0trx.cc` uses that variable when deciding
  whether to flush commit redo.

## Design

Keep the default at `MYLITE_DURABILITY_FULL`. When a caller supplies
`mylite_open_config.durability`, translate MyLite constants to MariaDB startup
options:

- `MYLITE_DURABILITY_FULL` -> `--innodb-flush-log-at-trx-commit=1`,
- `MYLITE_DURABILITY_NORMAL` -> `--innodb-flush-log-at-trx-commit=2`,
- `MYLITE_DURABILITY_OFF` -> `--innodb-flush-log-at-trx-commit=0`.

Store the selected durability on the process-global embedded runtime. A second
same-process handle can share an existing runtime only when it requests the
same durability policy; otherwise open fails with `MYLITE_BUSY` because the
already-started MariaDB runtime cannot change its startup flush option.

Extend the embedded performance probe with `MYLITE_PERF_DURABILITY` so local
runs can compare startup and engine throughput at `FULL`, `NORMAL`, and `OFF`
without changing code. Add an autocommit insert loop because the existing
transactional insert loop amortizes commit sync across all rows and does not
show the cost that WordPress-style DDL/autocommit-heavy workloads pay. CI
continues to use the default full durability probe.

## Compatibility Impact

Default behavior is unchanged. Existing callers that pass no config, a partial
config without `durability`, or `MYLITE_DURABILITY_FULL` still receive full
commit durability. Callers that already requested `NORMAL` or `OFF` now receive
the documented weaker durability mode instead of being silently upgraded to
full durability.

## Directory And Lifecycle Impact

No directory-layout change. Runtime startup still creates and cleans the same
database-owned `run/` and `tmp/` directories.

## Native Storage Impact

No native file-format change. The slice only selects an existing MariaDB InnoDB
redo flush policy at startup.

## Build And Performance Impact

The default build and CI path stay on full durability. Opt-in NORMAL/OFF probe
runs can quantify how much write-heavy workloads are dominated by per-commit
syncs before deciding whether any test harness should intentionally use a
weaker durability mode. The added autocommit insert loop increases the embedded
probe slightly but keeps default iterations bounded by the existing
`MYLITE_PERF_INSERT_ITERATIONS` control.

## Test Plan

- Build `mylite_embedded_open_close_test` and
  `mylite_embedded_performance_probe`.
- Run the focused open/close baseline selector to verify FULL, NORMAL, OFF, and
  default durability mappings through
  `SELECT @@innodb_flush_log_at_trx_commit`, plus rejection of conflicting
  same-runtime durability policies.
- Run the embedded performance probe at default full durability.
- Run the embedded performance probe with `MYLITE_PERF_DURABILITY=NORMAL`.
- Run focused PHP CTest coverage to ensure the ordinary WordPress mysqli path
  still opens at default full durability.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-07 used the existing `embedded-dev` and
`php-embedded-dev` build trees.

- `cmake --build --preset embedded-dev --target
  mylite_embedded_open_close_test mylite_embedded_performance_probe` passed.
- `build/embedded-dev/packages/libmylite/mylite_embedded_open_close_test
  baseline` passed.
- `ctest --preset embedded-dev -R 'libmylite\.embedded-open-close$'
  --output-on-failure` passed.
- The default full-durability embedded performance probe passed and reported
  `mylite_perf_durability=FULL`, ordinary warm open/close `881.362ms`,
  ownerless warm open/close `640.352ms`, ordinary direct `SELECT 1`
  `3501.76 ops/s`, ownerless direct `SELECT 1` `3761.83 ops/s`, ordinary
  transactional inserts `1847.01 ops/s`, ordinary autocommit inserts
  `1475.79 ops/s`, ownerless transactional inserts `1136.67 ops/s`, and
  ownerless autocommit inserts `49.88 ops/s`.
- A reduced NORMAL durability probe with one open/close iteration, 100 select
  iterations, and 20 insert iterations passed and reported
  `mylite_perf_durability=NORMAL`.
- `cmake --build --preset php-embedded-dev --target mylite_php_extension
  mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- The public open config controls the embedded InnoDB flush policy.
- The default remains full durability.
- Conflicting durability policies are not silently ignored on shared
  same-process runtime opens.
- The performance probe prints the selected durability mode and autocommit
  insert throughput.
- No WordPress PHPUnit or PHP adapter durability default is weakened.
