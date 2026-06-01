# Ownerless Transaction Stress Trace Export

## Problem

The ownerless explicit transaction stress test covers concurrent independent
transactions, savepoint rollback, committed updates, final aggregate oracles,
and ownerless/native reopen checks. Phase 12 still calls for external
MariaDB/RQG-style long-running stress with oracles. A full external runner is
larger than one correctness slice, but the existing transaction/savepoint
schedule can be exported as SQL traces and final oracles so external harnesses
can replay the same workload against MariaDB-compatible targets.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc:102`, `:254`, `:650`, `:711`, and `:766`
  define `trans_begin()`, `trans_commit()`, `trans_savepoint()`,
  `trans_rollback_to_savepoint()`, and `trans_release_savepoint()`.
- `mariadb/sql/sql_parse.cc:5595-5695` dispatches `START TRANSACTION`,
  `COMMIT`, `ROLLBACK TO SAVEPOINT`, `SAVEPOINT`, and `RELEASE SAVEPOINT`.
- `mariadb/sql/sql_update.cc:346` defines
  `Sql_cmd_update::update_single_table()` for ordinary single-table SQL
  `UPDATE` execution.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_transaction_mix_stress()` creates one InnoDB table per
  worker, starts three concurrent workers, and verifies ownerless/native reopen
  before and after forced `.shm` rebuild.
- The same file's `ownerless_tx_stress_delta()`,
  `ownerless_tx_stress_delta_sum()`, `run_ownerless_tx_stress_worker()`, and
  `assert_ownerless_tx_stress_totals()` define the deterministic transaction
  schedule and final value, weighted-sum, version, and rollback oracle.

## Scope And Non-Goals

In scope:

- Add a dependency-free repository tool that writes SQL traces matching the
  deterministic ownerless transaction/savepoint stress schedule.
- Emit schema SQL, one worker SQL file per transaction worker, expected final
  oracle SQL, and a manifest.
- Add a CTest smoke check that generates a small trace and validates expected
  files and oracle fields.
- Update compatibility and ownerless-concurrency docs to mark transaction
  stress trace export as covered while full external oracle execution remains
  planned.

Out of scope:

- Starting or managing an external MariaDB server.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or the in-process transaction stress
  schedule.
- Adding retry logic; the exported workers use independent tables and should
  not need deadlock/timeout retry in normal external execution.

## Design

`tools/ownerless-transaction-stress-trace` accepts `--output DIR`,
`--rounds N`, and `--check`. It writes:

- `schema.sql`, which creates `app.ownerless_tx_stress_1` through
  `app.ownerless_tx_stress_3` with three rows each,
- `worker-1.sql` through `worker-3.sql`, each running the deterministic
  transaction loop: update row 1, set a savepoint, update row 2, roll back to
  the savepoint, verify row 2 is still zero, update row 3, release the
  savepoint, periodically re-check row 2, and commit,
- `expected.sql`, which reports per-worker aggregate state and returns `ok`
  only when value sums, weighted sums, versions, and rolled-back row state match
  the deterministic oracle,
- `manifest.txt`, which records constants and expected totals.

External harnesses can run `schema.sql`, execute worker files concurrently,
then run `expected.sql`. The smoke CTest uses `--check` to prove trace
generation without requiring an external server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The generated trace makes the existing deterministic
transaction/savepoint stress schedule portable to external MariaDB/RQG-style
validation.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL uses ordinary
MariaDB DDL, DML, transaction, and savepoint statements so an external runner
can own its own schema and data directory.

## Native Storage Impact

No native storage runtime changes. When replayed externally, the trace covers
ordinary InnoDB transaction commit and savepoint rollback over independent
tables.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds a shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-transaction-stress-trace --rounds 3 --output <tmp>
  --check`.
- Run `ctest --preset embedded-dev -R tools.ownerless-transaction-stress-trace
  --output-on-failure`.
- Run focused ownerless transaction stress to prove the source schedule still
  passes.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, worker, expected, and manifest files.
- The smoke test verifies deterministic transaction, savepoint rollback, and
  aggregate oracle fields are emitted.
- Documentation and compatibility notes mark transaction trace export as
  covered while full external MariaDB/RQG execution remains planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas;
  future changes to those formulas must update the tool in the same slice.
- The generated files are external harness inputs, not a replacement for
  actually running long-lived MariaDB/RQG workloads.
