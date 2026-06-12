# Ownerless Prepared DML Native Prepare Profile

## Problem

Ownerless prepared no-result DML deliberately creates and closes the native
MariaDB `MYSQL_STMT` inside each `mylite_step()` execution. That fixed the
prepared-DML native lifetime failure where process-local InnoDB prepare state
could survive outside the ownerless statement boundary and interfere with
peers.

The production embedded performance probe now shows ownerless startup and
active reconnect are close to ordinary baselines, while ownerless prepared
insert throughput remains materially below ordinary inserts. The existing
prepared-step counters time the whole ownerless statement wrapper and
`mysql_stmt_execute()`, but they do not isolate the per-step native prepare and
close cost introduced for correctness.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` marks ownerless ordinary no-result
  `INSERT`, `UPDATE`, `DELETE`, and `REPLACE` prepared statements as
  `ownerless_native_prepare_per_step`.
- `mylite_step()` calls `prepare_ownerless_ephemeral_native_statement()` after
  ownerless statement locks and refreshes are in place. The scoped cleanup then
  calls `close_ownerless_ephemeral_native_statement()` before returning.
- `mysql_stmt_prepare()` is still the native MariaDB authority for syntax,
  table, column, and engine diagnostics for this ownerless DML subset; the
  public MyLite object stores SQL text and parameter count before native
  prepare.
- The existing continuous single-owner proof does not prevent a new ownerless
  peer from joining after a native prepared handle is cached. It therefore
  cannot by itself justify keeping a native prepared DML handle alive across
  public MyLite calls.

## Design

Add stats-gated ownerless database performance counters for:

- ownerless prepared-DML native prepare calls,
- native prepare elapsed time,
- native close calls, and
- native close elapsed time.

The counters are emitted by `mylite_embedded_performance_probe` in the raw
ownerless insert phase output and as ownerless autocommit per-insert summaries.
They are enabled only by the existing
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` diagnostic mode.

This slice intentionally does not cache native prepared DML handles. A safe
cache needs a peer-join barrier or another directory-owned proof that no peer
can join while process-local native prepare state is retained. The current
single-owner snapshot proof only proves the state at the time it is checked.

## Compatibility Impact

No SQL behavior, public C API behavior, PHP/mysqli behavior, metadata,
directory layout, native storage format, or ownerless locking semantics change.
Ownerless prepared no-result DML continues to create native MariaDB prepared
state inside the protected execution interval and close it before returning.

## Native Storage Impact

No native InnoDB page, redo, undo, dictionary, or checkpoint behavior changes.
The slice only measures existing native prepare/close work.

## Build And Performance Impact

Stats-off behavior is unchanged. Stats-enabled ownerless prepared DML adds
relaxed counter updates and clock reads around already-expensive native
prepare/close calls. The expected value is attribution: if native prepare is a
large per-insert cost, the next optimization must first design a safe peer-join
barrier or a different parameterized execution path.

## Test And Verification Plan

- Build production embedded targets for `mylite_embedded_performance_probe`,
  `mylite_embedded_prepared_statement_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run focused prepared statement coverage.
- Run a reduced stats-enabled production performance probe and confirm the new
  native prepare/close keys are emitted.
- Run a reduced stats-off production performance probe to keep the throughput
  signal separate from attribution overhead.
- Run focused ownerless checksum-stress controls with prepared writers to
  ensure this diagnostic slice did not weaken the per-step native lifetime
  boundary.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- The performance probe emits native prepare and close calls/time for ownerless
  prepared DML when stats are enabled.
- Stats-off output remains unchanged for these attribution keys.
- Prepared DML reset/reexecution and duplicate-key recovery still pass.
- Checksum stress with prepared writers still reaches the ownerless execution
  boundary without reviving the native prepared lifetime failure.
- Docs record why a single-owner point-in-time proof is not enough to cache
  native prepared DML handles across peer joins.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_embedded_prepared_statement_test
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-prepared-statement$' --output-on-failure` passed.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`,
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`, and
  `MYLITE_PERF_INSERT_ITERATIONS=100` emitted
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_prepare_calls=100`,
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_prepare_ms=41.498`,
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_close_calls=100`,
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_close_ms=29.987`,
  `mylite_perf_summary_ownerless_autocommit_prepared_native_prepare_calls_per_insert=1.000`,
  `mylite_perf_summary_ownerless_autocommit_prepared_native_prepare_ms_per_insert=0.415`,
  `mylite_perf_summary_ownerless_autocommit_prepared_native_close_calls_per_insert=1.000`,
  and
  `mylite_perf_summary_ownerless_autocommit_prepared_native_close_ms_per_insert=0.300`.
  In that run ordinary autocommit insert throughput was `3177.44 ops/s`,
  ownerless autocommit insert throughput was `414.21 ops/s`, and the
  ownerless/ordinary ratio was `0.1304`.
- A reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=500` emitted only the existing throughput
  summaries for this slice's keys. In that run ordinary transaction insert
  throughput was `1853.93 ops/s`, ownerless transaction insert throughput was
  `978.20 ops/s`, ordinary autocommit insert throughput was `1916.97 ops/s`,
  ownerless autocommit insert throughput was `645.03 ops/s`, and the
  ownerless autocommit ratio was `0.3365`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- Focused one-round `checksum-stress` controls passed with
  `MYLITE_OWNERLESS_CHECKSUM_STRESS_PREPARED_WRITERS=0`, `1`, and `2`,
  each under a 120-second timeout.

The attribution result is large enough to make native prepare/close lifecycle
cost a first-order ownerless prepared-DML performance target, but it does not
justify caching native `MYSQL_STMT` handles until peer joins are covered by a
directory-owned barrier or lease.

## Risks And Follow-Up

- Instrumentation does not improve throughput by itself.
- If native prepare is dominant, a future performance slice needs a safe
  peer-join barrier, a bounded statement-cache lease, or a text-protocol
  parameter execution path that avoids long-lived native prepare state.
- If native prepare is small, the next optimization should stay on
  page-version WAL volume, row-level MTR commit, or undo-report publication.
