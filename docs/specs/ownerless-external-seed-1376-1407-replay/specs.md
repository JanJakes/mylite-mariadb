# Ownerless External Seed 1376-1407 Replay

## Problem Statement

Ownerless external stress evidence includes deterministic full trace replay,
fixed noncontiguous seed replay, dependency-free seed-sweep checks through seed
`1375`, and Docker-backed combined seed-sweep replay through seed `1375` for
the random transaction, DDL stress, and foreign-key graph seeded suites. Full
randomized MariaDB/RQG execution remains planned, but the next bounded evidence
slice is to keep widening deterministic generated-input coverage without
adding Docker to default CI.

This slice advances the dependency-free command-plan check window to seeds
`1376` through `1407`, and records Docker-backed MariaDB replay evidence for
the same seeds at two rounds across the random transaction, DDL stress, and
foreign-key graph seeded suites.

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
- Replay windows from `224` through `1375` used
  `--fk-graph-replay-attempts 10` for bounded FK graph raw-client deadlock
  recovery.

## Scope And Non-Goals

In scope:

- Add a production CTest command-plan check for seeds `1376` through `1407` at
  two rounds across all three seeded suites.
- Run one dependency-free command-plan check for seeds `1376` through `1407`
  at two rounds with `--fk-graph-replay-attempts 10`.
- Record one Docker-backed MariaDB 11.8 seed-sweep replay for seeds `1376`
  through `1407` at two rounds with `--fk-graph-replay-attempts 10`.
- Inspect replay manifest, final oracle, and FK graph retry evidence.
- Update compatibility and ownerless concurrency docs.

Out of scope:

- Adding a new SQL generator, RQG runner, SQLancer runner, shrinker, or default
  Docker dependency.
- Changing MyLite runtime behavior, storage layout, public APIs, native redo,
  page-version WAL, recovery, or directory lifecycle behavior.
- Claiming long-running randomized external MariaDB/RQG stress is complete.

## Design

The production CTest uses the combined wrapper in dependency-free check mode:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output <build-dir>/ownerless-external-mariadb-seed-sweep-next-check \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --fk-graph-replay-attempts 10 \
  --seed-range 1376:1407 \
  --check
```

The opt-in real-client replay keeps using the existing wrapper and explicit FK
graph retry budget:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output <output-dir> \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --fk-graph-replay-attempts 10 \
  --seed-range 1376:1407
```

Only the FK graph suite receives the extra `--replay-attempts` argument. Random
transaction and DDL stress replay remain unchanged. The manifest records
`suite_fk_graph_replay_attempts=10` so replay artifacts show the explicit retry
budget used for this window.

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
- Run the direct `1376:1407` check-mode seed sweep at two rounds with
  `--fk-graph-replay-attempts 10`.
- Inspect Docker-backed `1376:1407` replay artifacts for manifest,
  `expected.out` oracle files, empty `expected.err` files, and FK graph retry
  directories.
- Run focused production CTest coverage for the combined seed-sweep checks.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

- `bash -n tools/ownerless-external-mariadb-seed-sweep
  tools/ownerless-random-tx-seed-suite
  tools/ownerless-ddl-stress-seed-suite
  tools/ownerless-fk-graph-seed-suite` passed.
- The direct dependency-free check-mode command for seed range `1376:1407`,
  rounds `2`, and `--fk-graph-replay-attempts 10` passed with
  `ownerless_external_mariadb_seed_sweep_check=ok`.
- Docker-backed MariaDB 11.8 replay for seed range `1376:1407`, rounds `2`,
  and `--fk-graph-replay-attempts 10` passed with
  `ownerless_external_mariadb_seed_sweep=ok`.
- The replay manifest recorded `image=mariadb:11.8`, `suite_count=3`,
  `seed_count=32`, and `suite_fk_graph_replay_attempts=10`.
- Replay artifacts contained `96` `expected.out` files and `96`
  `expected.err` files; every `expected.err` file was empty and every final
  suite oracle ended in `ok`.
- FK graph replay recovered transient raw MariaDB `1213` deadlock exits for
  seed `1376` on attempt `2`, seed `1383` on attempt `8`, seed `1384` on
  attempt `4`, seed `1386` on attempt `2`, seed `1387` on attempt `3`, seed
  `1389` on attempt `2`, seed `1393` on attempt `2`, seed `1397` on attempt
  `2`, seed `1399` on attempt `3`, seed `1400` on attempt `2`, seed `1401`
  on attempt `2`, and seed `1407` on attempt `2`.

## Acceptance Criteria

- Production CTest includes a dependency-free combined seed-sweep check for
  seeds `1376` through `1407` at two rounds.
- The direct check-mode command validates seeds `1376` through `1407` at two
  rounds for all three suites with an explicit FK graph retry budget.
- Docker-backed MariaDB 11.8 replay artifacts prove random transaction, DDL
  stress, and FK graph seeded suites for seeds `1376` through `1407` at two
  rounds.
- The combined wrapper manifest records the FK graph replay-attempt budget.
- All `expected.out` oracle outputs contain a terminal `ok` row, and every
  `expected.err` file is empty.
- Documentation distinguishes this bounded deterministic replay from full
  randomized external MariaDB/RQG stress.

## Risks And Follow-Up

- This is deterministic generated-input replay, not full randomized RQG.
- FK graph retry evidence can still show ordinary raw MariaDB deadlock exits
  before the suite-level retry succeeds.
- The next correctness work remains live-peer DDL/file-lifecycle recovery and
  broader redo/checkpoint reconciliation.
