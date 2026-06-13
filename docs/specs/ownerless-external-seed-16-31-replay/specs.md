# Ownerless External Seed 16-31 Replay

## Problem Statement

Ownerless external stress evidence includes deterministic trace-suite replay,
fixed noncontiguous seed replay, dependency-free seed-sweep checks through
seed `31`, and Docker-backed seed-sweep replay through seed `15`. The remaining
external-oracle gap is still broader randomized MariaDB/RQG execution, but the
next bounded evidence slice is to replay the already-validated next contiguous
seed window through a real MariaDB 11.8 server.

This slice records Docker-backed MariaDB replay for seeds `16` through `31` at
two rounds across the random transaction, DDL stress, and foreign-key graph
seeded suites.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-external-mariadb-seed-sweep` already runs the random
  transaction, DDL stress, and FK graph seeded suites through one disposable
  MariaDB container by default.
- `tools.ownerless-external-mariadb-seed-sweep-wide-check` already validates
  the dependency-free command plan for seeds `0` through `31` at two rounds for
  all three suites, so this slice does not need another default CTest entry.
- `tools/ownerless-fk-graph-seed-suite` intentionally retries a whole seed when
  a raw external MariaDB client exits with transient `1205`/`1213` contention
  before the final oracle can run.

## Scope And Non-Goals

In scope:

- Run one Docker-backed MariaDB 11.8 seed sweep for seeds `16` through `31` at
  two rounds.
- Include random transaction, DDL stress, and FK graph seeded suites.
- Record manifest, final oracle, and FK graph retry evidence.
- Update compatibility and ownerless concurrency docs.

Out of scope:

- Adding a new SQL generator, RQG runner, SQLancer runner, shrinker, or default
  Docker dependency.
- Adding another default CI test for a command plan already covered by the
  existing wide check.
- Changing MyLite runtime behavior, storage layout, public APIs, or directory
  lifecycle behavior.
- Claiming long-running randomized external MariaDB/RQG stress is complete.

## Design

Use the existing combined wrapper without changing production code:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output /tmp/mylite-ownerless-external-seed-sweep-16-31-r2-replay \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --seed-range 16:31
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

- Run `bash -n` for the combined seed sweep and the suite wrappers.
- Run the direct `16:31` check-mode seed sweep at two rounds.
- Run the Docker-backed `16:31` replay at two rounds.
- Inspect the replay manifest, final `expected.out` files, empty
  `expected.err` files, and FK graph retry directories.
- Run the focused production CTest selector that includes the existing
  seed-sweep checks and all seeded suites.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The direct check-mode command validates seeds `16` through `31` at two rounds
  for all three suites.
- Docker-backed MariaDB 11.8 replay passes random transaction, DDL stress, and
  FK graph seeded suites for seeds `16` through `31` at two rounds.
- All final oracle outputs end in `ok`, and final `expected.err` files are
  empty.
- Documentation distinguishes this bounded deterministic replay from full
  randomized external MariaDB/RQG stress.

## Evidence

The direct dependency-free command-plan check passed for all three suites:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-16-31-r2-check --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 16:31 --check
seed=16
...
seed=31
check=ok
ownerless_external_mariadb_seed_sweep_check=ok
```

Docker-backed MariaDB 11.8 replay then passed the same window:

```text
tools/ownerless-external-mariadb-seed-sweep --output /tmp/mylite-ownerless-external-seed-sweep-16-31-r2-replay --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 16:31
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
fk_graph_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

The replay manifest recorded `mode=replay`, `suite_count=3`,
`random_tx_rounds=2`, `ddl_rounds=2`, `fk_graph_rounds=2`, `seed_count=16`,
and seeds `16` through `31`. All random transaction, DDL stress, and FK graph
final `expected.out` files ended in `ok`, and final `expected.err` files were
empty. FK graph seeds `17`, `21`, and `25` recovered on attempt `2` after raw
external MariaDB `1213` deadlock exits, matching the suite's bounded
whole-seed retry model.

## Risks And Follow-Up

- This is deterministic generated-input replay, not full randomized RQG.
- FK graph retry evidence shows the external client can still see ordinary raw
  MariaDB deadlock exits before the suite-level retry succeeds.
- The next correctness work remains live-peer DDL/file-lifecycle recovery and
  broader redo/checkpoint reconciliation.
