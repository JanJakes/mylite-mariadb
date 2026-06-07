# Ownerless Random Transaction Seed Suite

## Problem Statement

The deterministic ownerless SQL trace suite now has full scale-2 Docker-backed
MariaDB replay evidence for all trace families, but the remaining external
MariaDB/RQG gap still needs broader generated schedule coverage. The existing
random transaction trace had one fixed pseudo-random schedule. A full RQG
runner is larger than this slice, but the random transaction exporter can expose
multiple deterministic seed variants and run them through the common trace
runner.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_random_transaction_stress()` and
  `run_ownerless_random_tx_stress_worker()` define the product stress shape:
  four ownerless workers over worker-owned row partitions, explicit
  transactions, savepoint rollback, full transaction rollback, bounded retry for
  MariaDB 1205/1213 lock errors, aggregate-reader bounds, final aggregate
  oracles, forced `.shm` rebuild, and native exclusive reopen.
- `tools/ownerless-random-tx-trace` already mirrors the seed-0 product schedule
  without opening MyLite. Its generated SQL is ordinary MariaDB DDL/DML and can
  be replayed by `tools/ownerless-sql-trace-runner`.

## Design

Add `--seed N` to `tools/ownerless-random-tx-trace`:

- Seed `0` is the default and preserves the existing product stress formulas.
- Nonzero seeds deterministically shift worker-local row selection, rollback
  decisions, and update deltas while preserving the same table, worker count,
  transaction/savepoint structure, and aggregate oracle shape.
- `manifest.txt` records `seed=N` and
  `schedule=ownerless_random_tx_seeded_v1` so external logs identify the exact
  generated variant.

Add `tools/ownerless-random-tx-seed-suite`:

- It accepts repeatable `--seed N`, defaulting to seeds `0`, `17`, and `83`.
- It generates one trace directory per seed under `seed-<N>/`.
- In `--check` mode it validates every generated trace through the common
  `ownerless-sql-trace-runner --check` path without Docker or a SQL server.
- In replay mode it forwards `--client`, repeated `--client-arg`, and
  `--log-dir` to the common runner for each seed.
- It writes `seed-suite-manifest.txt` with rounds and seed list.

The seed suite is intentionally separate from `ownerless-sql-trace-suite` so the
11-family deterministic suite count remains stable. Seeded random transaction
variants are an additional generated external-oracle bridge, not a replacement
for long-running randomized RQG/SQLancer.

The later `ownerless-seed211-trace-checks` slice keeps the default seed suite
and Docker replay evidence at `0`, `17`, and `83`, but adds seed `211` to the
dependency-free CTest check-mode invocation for additional deterministic input
variation.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, runtime behavior, or default
ownerless stress behavior changes. The change expands external compatibility
evidence by making deterministic random transaction variants reproducible and
replayable through the existing MariaDB-compatible trace runner.

## Database Directory And Lifecycle Impact

No MyLite database-directory changes. Generated traces and replay logs live
under caller-provided output directories; replay mutates only the target SQL
server's `app` schema.

## Native Storage Impact

No MyLite native storage format changes.

## Public API, Build, Size, And Dependencies

No public API, production build, binary-size, license, or dependency changes.
The new tool is a dependency-free Bash script used by CTest and optional
external replay.

## Test Plan

- Run `bash -n tools/ownerless-random-tx-trace
  tools/ownerless-random-tx-seed-suite`.
- Run seed-0 and nonzero seed exporter checks.
- Run `tools/ownerless-random-tx-seed-suite --rounds 3 --seed 0 --seed 17
  --seed 83 --seed 211 --check`.
- Run focused CTest coverage for `tools.ownerless-random-tx-trace`,
  `tools.ownerless-random-tx-seed-suite`, and scaled random trace-suite checks.
- If Docker is available, run the seed suite through a disposable MariaDB 11.8
  client/server.
- Run `cmake --build --preset format-check`, `git diff --check`, cached diff
  checks, and cleanup checks.

## Acceptance Criteria

- Seed `0` keeps the existing round-3 oracle values.
- Nonzero seeds produce distinct deterministic oracle values.
- The seed suite validates every requested seed through the common trace-runner
  plan.
- Optional external MariaDB replay uses the same generated seed directories and
  final aggregate oracles.
- Docs continue to mark long-running randomized external MariaDB/RQG stress as
  planned.

## Evidence

Dependency-free check-mode validation passed for rounds `3` and seeds `0`,
`17`, `83`, and, in the later seed-211 follow-up, `211`:

```text
seed_count=4
seed=0
seed=17
seed=83
seed=211
random_tx_seed_suite_check=ok
```

Focused Docker-backed MariaDB 11.8 replay is recorded for rounds `8` and
seeds `0`, `17`, `83`, and, in the later seed-211 external replay follow-up,
`211`:

```text
seed_count=4
seed=0
seed=17
seed=83
seed=211
random_tx_seed_suite_run=ok
external_random_tx_seed_smoke=ok
```

The final MariaDB oracle logs reported `ok` for every seed, with empty
`expected.err` files:

```text
seed=0 observed_count=16 observed_sum=19336794 observed_versions=77 observed_weighted_sum=204322890 ownerless_random_tx_trace_check=ok
seed=17 observed_count=16 observed_sum=1328136094 observed_versions=77 observed_weighted_sum=11267818492 ownerless_random_tx_trace_check=ok
seed=83 observed_count=16 observed_sum=6410236494 observed_versions=77 observed_weighted_sum=54566507669 ownerless_random_tx_trace_check=ok
seed=211 observed_count=16 observed_sum=16266336694 observed_versions=77 observed_weighted_sum=137984903736 ownerless_random_tx_trace_check=ok
```

The later `ownerless-external-seed-sweep` slice adds a combined external
wrapper and records focused MariaDB 11.8 replay of this suite alongside the DDL
seed suite for seeds `0` through `7` at rounds `4`. The
`ownerless-external-seed-range-checks` follow-up broadens dependency-free
combined seed-sweep validation to seeds `0` through `15` at rounds `3`, while
keeping true randomized RQG/SQLancer stress planned.

## Risks And Unresolved Questions

- Seeded traces broaden deterministic input schedules but are still not a true
  randomized RQG generator.
- The seeded variants preserve worker-owned row partitions, so they do not add
  new cross-worker row-lock conflict classes beyond the product random
  transaction stress shape. Conflict-heavy coverage remains in the ownerless
  row/gap/deadlock and FK graph stress tests.
