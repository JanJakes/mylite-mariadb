# Ownerless Random Transaction Trace Export

## Problem

The ownerless stress preset contains deterministic checksum, pseudo-random
transaction, DDL, temporary-table, foreign-key graph, and active-reader pressure
stress. The remaining Phase 12 validation gap is external MariaDB/RQG-style
oracle stress. A full external runner is larger than a correctness slice, but
the current pseudo-random transaction stress can expose a stable SQL trace and
aggregate oracle so external runners can replay the same schedule against
MariaDB-compatible targets.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_random_transaction_stress()` drives four deterministic
  ownerless workers over disjoint row partitions, using transaction rollback,
  savepoint rollback, aggregate-reader bounds, and final aggregate oracles.
- The same file's `ownerless_random_tx_stress_rows()`,
  `ownerless_random_tx_stress_delta()`, and expected-total helpers define a
  pure deterministic schedule that can be emitted without opening MyLite.

## Scope And Non-Goals

In scope:

- Add a repository tool that writes SQL traces matching the deterministic random
  transaction stress schedule.
- Emit schema, one SQL file per worker, expected aggregate SQL, and a manifest.
- Add a CTest smoke check that generates a small trace and verifies the expected
  oracle fields are present.
- Document that this is external-oracle scaffolding, not completion of the
  external MariaDB/RQG stress requirement.

Out of scope:

- Starting external MariaDB servers.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or stress semantics.

## Design

`tools/ownerless-random-tx-trace` is a dependency-free shell tool. It accepts
`--output DIR`, `--rounds N`, `--seed N`, and `--check`. Seed `0` is the
default and preserves the ownerless C stress formulas exactly. Nonzero seeds
derive deterministic external-oracle variants by shifting row selection,
rollback points, and deltas while preserving the same four-worker
transaction/savepoint shape. It writes:

- `schema.sql`, which creates `app.ownerless_random_tx_stress` and inserts the
  same initial rows as the ownerless C stress test,
- `worker-1.sql` through `worker-4.sql`, each containing the deterministic
  transaction/savepoint/rollback schedule for that worker,
- `expected.sql`, which reports observed aggregates and returns `ok` only when
  count, sum, version count, and weighted sum match the deterministic oracle,
- `manifest.txt`, which records constants, seed, schedule version, and expected
  totals.

External harnesses can run `schema.sql`, execute the worker files concurrently,
then run `expected.sql` against MariaDB or another target. The CTest smoke path
uses `--check` to validate generation without requiring an external server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior changes.
The generated trace makes the existing deterministic ownerless stress schedule
portable to external MariaDB/RQG-style validation. Seeded nonzero traces expand
generated external-oracle schedules; they do not change product ownerless
runtime behavior or replace full randomized RQG.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL intentionally
uses ordinary MariaDB DDL and DML so an external runner can create its own test
schema.

## Test Plan

- Run `tools/ownerless-random-tx-trace --rounds 3 --seed 0 --output <tmp>
  --check`.
- Run `tools/ownerless-random-tx-trace --rounds 3 --seed 17 --output <tmp>
  --check`.
- Run `ctest --preset embedded-dev -R tools.ownerless-random-tx-trace
  --output-on-failure`.
- Run `cmake --build --preset format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, worker, expected, and manifest files.
- The smoke test verifies deterministic aggregate oracle values are emitted.
- Seed `0` remains compatible with the existing product stress formulas, and
  nonzero seeds produce distinct deterministic oracle totals.
- Documentation and compatibility notes still mark full external MariaDB/RQG
  stress as planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas; future
  changes to those formulas must update the seed-0 path in the same slice.
- The generated worker files are an input to external stress, not an external
  stress runner by themselves.
