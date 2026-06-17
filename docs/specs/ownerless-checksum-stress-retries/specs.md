# Ownerless Checksum Stress Retries

## Problem

The production ownerless stress preset can fail the shared-table checksum stress
when a direct DML writer receives `MYLITE_BUSY` with the diagnostic
`ownerless table write statement lock is busy`. The checksum stress intentionally
starts multiple ownerless writers against one table, so contention on the
directory-owned ownerless table-write statement lock is expected. The test
harness currently treats that transient contention as a fatal assertion even
though adjacent random-transaction and foreign-key graph stress cases already
use bounded retries for lock wait timeouts and deadlocks.

This makes the stress preset noisy and can hide performance or correctness work
behind an expected contention class.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` maps ownerless statement-lock timeout to
  `MYLITE_BUSY` and the message `ownerless table write statement lock is busy`.
  `SET lock_wait_timeout = N` updates the MyLite ownerless statement-lock wait
  timeout for the handle.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `run_ownerless_checksum_stress_writer()` and
  `run_ownerless_prepared_checksum_stress_writer()` previously set
  `lock_wait_timeout = 30` and asserted every single update succeeded on its
  first attempt.
- The same test file already uses bounded retry loops in
  `run_ownerless_random_tx_stress_worker()` and
  `run_ownerless_fk_graph_stress_worker()` for MariaDB lock wait timeout `1205`
  and deadlock `1213`.
- The checksum stress has strong final oracles: sum, version count, weighted
  sum, ownerless reopen, native reopen, forced `.shm` rebuild, and final
  no-peer WAL reclamation. Retrying a failed single-statement update does not
  weaken those oracles; if a failed attempt had applied its update anyway, the
  final checksum would fail.

## Design

Add checksum-stress-specific bounded retry helpers:

- Direct DML updates retry when `mylite_exec()` returns `MYLITE_BUSY`, MariaDB
  lock wait timeout `1205`, or MariaDB deadlock `1213`.
- Prepared DML updates retry when `mylite_step()` returns `MYLITE_BUSY` or the
  owning database reports MariaDB lock wait timeout `1205` or deadlock `1213`.
  A retryable failed prepared step is reset before the next attempt.
- Checksum writer sessions use fail-fast MyLite statement-lock attempts
  (`lock_wait_timeout = 0`) plus one-second InnoDB waits, so the retry loop
  bounds CI wall time instead of sleeping for the previous 30-second ownerless
  statement-lock wait on each transient collision.
- The checksum retry budget is larger than the random/FK transaction retry
  budget because fail-fast statement-lock attempts can collide many times while
  another worker holds the same table write statement lock.
- Retry delay uses a deterministic worker/round/attempt backoff, matching the
  existing stress style.
- Exhausted retries still fail loudly with worker, round, and SQL context.
- The registered ownerless-stress CTest keeps the checksum stress at the
  default 48 rounds. Larger same-table checksum runs remain available through
  `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS`, but they are not CI-gating until
  ownerless same-table writer fairness is improved.

The product ownerless statement-lock semantics are unchanged. The stress test
still proves every logical checksum update happens exactly once through its
existing final aggregate oracles.

## Scope And Non-Goals

In scope:

- Checksum stress harness retry policy for transient ownerless/MariaDB
  contention.
- Documentation that checksum stress is a contention-retrying stress case.
- Verification of the full checksum stress under the production stress preset.

Out of scope:

- Changing ownerless statement-lock wait behavior or fairness.
- Changing `lock_wait_timeout` semantics or public error reporting.
- Changing checksum stress SQL shape, row schedule, or final aggregate oracles.
- Broader performance work on ownerless same-table writers.

## Compatibility Impact

No product SQL, C API, PHP API, wire-protocol, storage-format, or directory
layout behavior changes. The slice changes only test harness behavior for a
stress workload that already exercises expected lock contention.

## Database Directory And Lifecycle Impact

No durable layout changes. The stress test continues to create all runtime and
database files under the existing temporary MyLite-owned test directory and
continues to verify ownerless/native reopen and forced shared-memory rebuild.

## Native Storage Impact

Native InnoDB locking and recovery behavior are unchanged. The test remains a
mixed direct/prepared DML workload over a native InnoDB table.

## Build, Size, And Dependency Impact

No production binary or dependency impact. The change affects only the
`mylite_ownerless_cross_process_sql_test` test executable.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-stress`.
- Run `ctest --preset ownerless-stress -R
  libmylite.ownerless-cross-process-checksum-stress --output-on-failure`.
- Run the remaining ownerless stress subset if the checksum stress had failed
  in the same local verification sequence.
- Run focused production ownerless proof selectors as a smoke check only if
  first-party code outside the test harness changes.
- Run format and whitespace checks.

## Verification Results

Local verification on 2026-06-17 used the production `MinSizeRel` MariaDB
embedded archive and first-party `Release` builds:

- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` rebuilt the stress executable.
- The first retry attempt with one-second statement-lock waits passed
  `libmylite.ownerless-cross-process-checksum-stress`, but took `523.73`
  seconds, which was not acceptable for CI signal.
- A fail-fast statement-lock attempt with only `200` retries exhausted in about
  `5` seconds; the final design uses `5000` fail-fast attempts and registers
  the checksum stress at the default `48` rounds.
- `ctest --preset ownerless-stress -R
  libmylite.ownerless-cross-process-checksum-stress --output-on-failure`
  passed twice with the registered `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=48`
  setting, with observed runtimes of `55.13` seconds and `64.36` seconds.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Acceptance Criteria

- Checksum stress retries transient `MYLITE_BUSY`, MariaDB `1205`, and MariaDB
  `1213` without weakening final aggregate/reopen/rebuild oracles.
- Exhausted checksum retries still fail with useful worker/round diagnostics.
- The production ownerless checksum stress CTest passes with the registered
  `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=48`.

## Risks And Unresolved Questions

- Retrying contention makes the stress test less brittle, but it does not solve
  any underlying ownerless statement-lock fairness or performance issue. That
  remains a separate product/performance slice if lock waits become frequent or
  visible in application workloads.
- The prepared-statement retry path depends on `mylite_reset()` being valid
  after a retryable failed step. Focused stress verification must cover that
  path with the default two prepared checksum writers.
