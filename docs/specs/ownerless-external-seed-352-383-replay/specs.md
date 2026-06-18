# Ownerless External Seed 352-383 Replay

## Problem Statement

Ownerless external stress evidence includes deterministic full trace replay,
fixed noncontiguous seed replay, dependency-free seed-sweep checks through seed
`63`, and Docker-backed combined seed-sweep replay through seed `351` for the
random transaction, DDL stress, and foreign-key graph seeded suites.
Full randomized MariaDB/RQG execution remains planned, but the next bounded
evidence slice is to replay the next contiguous seed window through a real
MariaDB 11.8 server.

This slice records Docker-backed MariaDB replay for seeds `352` through `383`
at two rounds across the random transaction, DDL stress, and foreign-key graph
seeded suites. The replay uses the combined wrapper's explicit FK graph
whole-seed retry budget so ordinary raw-client `1205`/`1213` contention can
recover without changing the random transaction or DDL replay behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-external-mariadb-seed-sweep` runs the random transaction,
  DDL stress, and FK graph seeded suites through one disposable MariaDB
  container by default.
- `tools/ownerless-external-mariadb-seed-sweep --check` validates the same
  command plan without Docker or an external server.
- `tools/ownerless-fk-graph-seed-suite` retries a whole seed when a raw
  external MariaDB client exits with transient `1205`/`1213` contention before
  the final oracle can run.
- The previous replay windows from `224` through `351` used
  `--fk-graph-replay-attempts 10` for bounded FK graph raw-client deadlock
  recovery.

## Scope And Non-Goals

In scope:

- Run one dependency-free command-plan check for seeds `352` through `383` at
  two rounds with `--fk-graph-replay-attempts 10`.
- Run one Docker-backed MariaDB 11.8 seed sweep for seeds `352` through `383`
  at two rounds with `--fk-graph-replay-attempts 10`.
- Include random transaction, DDL stress, and FK graph seeded suites.
- Record manifest, final oracle, and FK graph retry evidence.
- Update compatibility and ownerless concurrency docs.

Out of scope:

- Adding a new SQL generator, RQG runner, SQLancer runner, shrinker, or default
  Docker dependency.
- Changing MyLite runtime behavior, storage layout, public APIs, or directory
  lifecycle behavior.
- Claiming long-running randomized external MariaDB/RQG stress is complete.

## Design

The combined wrapper runs the existing seeded suites without changing their SQL
generators:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output <output-dir> \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --fk-graph-replay-attempts 10 \
  --seed-range 352:383
```

Only the FK graph suite receives the extra `--replay-attempts` argument. Random
transaction and DDL stress replay remain unchanged. The manifest records
`suite_fk_graph_replay_attempts=10` so replay artifacts show the explicit retry
budget used for this window.

The real-client evidence window uses one disposable `mariadb:11.8` container
and writes a self-describing manifest plus per-suite trace and log directories.
The dependency-free direct check remains useful for fast validation, while the
Docker replay provides real-client oracle evidence.

## Compatibility Impact

No product SQL behavior changes. The slice broadens deterministic external
MariaDB evidence for generated random transaction, DDL stress, and FK graph
inputs while keeping full randomized external MariaDB/RQG stress planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Check-mode artifacts are written
under the caller-provided output directory. Replay artifacts are written under
the caller-provided output directory, and the disposable MariaDB container is
removed by default.

## Native Storage Impact

No MyLite native-storage format changes. The replay exercises unmodified
MariaDB 11.8 native InnoDB through generated SQL and final oracles.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes.
Default CTest remains dependency-free. Docker is required only for the optional
external replay command.

## Verification Plan

- Run `bash -n` for the combined seed sweep and suite wrappers.
- Run the direct `352:383` check-mode seed sweep at two rounds with
  `--fk-graph-replay-attempts 10`.
- Run the Docker-backed `352:383` replay at two rounds with
  `--fk-graph-replay-attempts 10`.
- Inspect the replay manifest, final `expected.out` files, empty
  `expected.err` files, and FK graph retry directories.
- Run focused production CTest coverage for the combined seed-sweep checks and
  fixed seeded-suite checks.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

- `bash -n tools/ownerless-external-mariadb-seed-sweep
  tools/ownerless-random-tx-seed-suite
  tools/ownerless-ddl-stress-seed-suite
  tools/ownerless-fk-graph-seed-suite
  tools/ownerless-external-mariadb-random-tx-seed-smoke
  tools/ownerless-external-mariadb-ddl-seed-smoke
  tools/ownerless-external-mariadb-fk-graph-seed-smoke`
  passed.
- `tools/ownerless-external-mariadb-seed-sweep --output
  build/manual-external-seed-sweep-352-383-r2-attempt10-check
  --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2
  --fk-graph-replay-attempts 10 --seed-range 352:383 --check`
  passed and ended with `ownerless_external_mariadb_seed_sweep_check=ok`.
- `tools/ownerless-external-mariadb-seed-sweep --output
  /tmp/mylite-ownerless-external-seed-sweep-352-383-r2-attempt10-replay
  --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2
  --fk-graph-replay-attempts 10 --seed-range 352:383` passed through a
  disposable `mariadb:11.8` container and ended with
  `random_tx_seed_suite_run=ok`, `ddl_stress_seed_suite_run=ok`,
  `fk_graph_seed_suite_run=ok`, and
  `ownerless_external_mariadb_seed_sweep=ok`.
- The replay manifest records `mode=replay`, `suite_count=3`,
  `suite=random-tx`, `suite=ddl`, `suite=fk-graph`, rounds `2` for all three
  suites, `suite_fk_graph_replay_attempts=10`, `seed_count=32`, and seeds
  `352` through `383`.
- Replay artifact inspection found `96` final `expected.out` files and `96`
  final `expected.err` files. Every final `expected.out` ended in `ok`, and
  every final `expected.err` was empty.
- FK graph replay recovered ordinary raw MariaDB deadlock exits under the
  whole-seed retry budget: seed `354` failed attempts `1`, `2`, and `3` with
  `ERROR 1213 (40001)` and passed attempt `4`; seeds `357` and `365` failed
  attempts `1` and `2` with `ERROR 1213 (40001)` and passed attempt `3`; seeds
  `358`, `362`, `363`, `368`, `369`, `371`, `375`, and `379` failed attempt
  `1` with `ERROR 1213 (40001)` and passed attempt `2`.

## Acceptance Criteria

- The direct check-mode command validates seeds `352` through `383` at two
  rounds for all three suites with an explicit FK graph retry budget.
- Docker-backed MariaDB 11.8 replay passes random transaction, DDL stress, and
  FK graph seeded suites for seeds `352` through `383` at two rounds.
- The combined wrapper manifest records the FK graph replay-attempt budget.
- All final oracle outputs end in `ok`, and final `expected.err` files are
  empty.
- Documentation distinguishes this bounded deterministic replay from full
  randomized external MariaDB/RQG stress.

## Risks And Follow-Up

- This is deterministic generated-input replay, not full randomized RQG.
- FK graph retry evidence can still show ordinary raw MariaDB deadlock exits
  before the suite-level retry succeeds.
- The next correctness work remains live-peer DDL/file-lifecycle recovery and
  broader redo/checkpoint reconciliation.
