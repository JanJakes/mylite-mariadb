# Ownerless External Seed 96-127 Replay

## Problem Statement

Ownerless external stress evidence currently includes deterministic full trace
replay, fixed noncontiguous seed replay, dependency-free seed-sweep checks
through seed `31`, Docker-backed replay through seed `95`, and focused
seed-suite replay for random transaction, DDL stress, and foreign-key graph
inputs. The remaining external-oracle gap is still full randomized
MariaDB/RQG execution, but the next bounded evidence slice is to replay the
next contiguous seed window through a real MariaDB 11.8 server.

This slice records Docker-backed MariaDB replay for seeds `96` through `127`
at two rounds across the random transaction, DDL stress, and foreign-key graph
seeded suites.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-external-mariadb-seed-sweep` runs the random transaction,
  DDL stress, and FK graph seeded suites through one disposable MariaDB
  container by default.
- `tools/ownerless-external-mariadb-seed-sweep --check` validates the same
  command plan without Docker or an external server.
- `tools/ownerless-random-tx-seed-suite`,
  `tools/ownerless-ddl-stress-seed-suite`, and
  `tools/ownerless-fk-graph-seed-suite` generate and validate the per-seed SQL
  traces before any external replay.
- `tools/ownerless-fk-graph-seed-suite` retries a whole seed when a raw
  external MariaDB client exits with transient `1205`/`1213` contention before
  the final oracle can run.

## Scope And Non-Goals

In scope:

- Run one dependency-free command-plan check for seeds `96` through `127` at
  two rounds.
- Run one Docker-backed MariaDB 11.8 seed sweep for seeds `96` through `127`
  at two rounds.
- Include random transaction, DDL stress, and FK graph seeded suites.
- Record manifest, final oracle, and FK graph retry evidence.
- Update compatibility and ownerless concurrency docs.

Out of scope:

- Adding a new SQL generator, RQG runner, SQLancer runner, shrinker, or
  default Docker dependency.
- Adding new default CTest coverage for this optional Docker replay.
- Changing MyLite runtime behavior, storage layout, public APIs, or directory
  lifecycle behavior.
- Claiming long-running randomized external MariaDB/RQG stress is complete.

## Design

Use the existing combined wrapper without changing production code:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output /tmp/mylite-ownerless-external-seed-sweep-96-127-r2-replay \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --seed-range 96:127
```

The replay uses one disposable `mariadb:11.8` container and writes a
self-describing manifest plus per-suite trace and log directories. The
dependency-free direct check remains useful for fast validation, while the
Docker replay provides real-client oracle evidence.

## Compatibility Impact

No product SQL behavior changes. The slice broadens external MariaDB evidence
for generated random transaction, DDL stress, and FK graph inputs while keeping
full randomized external MariaDB/RQG stress planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Replay artifacts are written
under the caller-provided output directory, and the disposable MariaDB
container is removed by default.

## Native Storage Impact

No MyLite native-storage format changes. The replay exercises unmodified
MariaDB 11.8 native InnoDB through generated SQL and final oracles.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes.
Default CTest remains dependency-free. Docker is required only for the optional
external replay command.

## Verification Plan

- Run `bash -n` for the combined seed sweep and suite wrappers.
- Run the direct `96:127` check-mode seed sweep at two rounds.
- Run the Docker-backed `96:127` replay at two rounds.
- Inspect the replay manifest, final `expected.out` files, empty
  `expected.err` files, and FK graph retry directories.
- Run `tools/check-ci-production-builds`.
- Run `git diff --check`.

## Verification Results

Completed.

## Evidence

The direct dependency-free command-plan check passed for all three suites:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-96-127-r2-check --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 96:127 --check
seed=96
...
seed=127
check=ok
ownerless_external_mariadb_seed_sweep_check=ok
```

Docker-backed MariaDB 11.8 replay then passed the same window:

```text
tools/ownerless-external-mariadb-seed-sweep --output /tmp/mylite-ownerless-external-seed-sweep-96-127-r2-replay --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 96:127
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
fk_graph_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

The replay manifest recorded `image=mariadb:11.8`, `mode=replay`,
`suite_count=3`, `random_tx_rounds=2`, `ddl_rounds=2`,
`fk_graph_rounds=2`, `seed_count=32`, and seeds `96` through `127`. All
random transaction, DDL stress, and FK graph final `expected.out` files ended
in `ok`, and all final `expected.err` files were empty. FK graph seeds `106`,
`113`, and `118` recovered on attempt `2` after raw external MariaDB `1213`
deadlock exits, matching the suite's bounded whole-seed retry model.

## Acceptance Criteria

- The direct check-mode command validates seeds `96` through `127` at two
  rounds for all three suites.
- Docker-backed MariaDB 11.8 replay passes random transaction, DDL stress, and
  FK graph seeded suites for seeds `96` through `127` at two rounds.
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
