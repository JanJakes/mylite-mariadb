# Ownerless External Seed 288-319 Replay

## Problem Statement

Ownerless external stress evidence includes deterministic full trace replay,
fixed noncontiguous seed replay, dependency-free seed-sweep checks through seed
`63`, Docker-backed combined seed-sweep replay through seed `287`, and focused
seed-suite replay for random transaction, DDL stress, and foreign-key graph
inputs. Full randomized MariaDB/RQG execution remains planned, but the next
bounded evidence slice is to replay the next contiguous seed window through a
real MariaDB 11.8 server.

This slice records Docker-backed MariaDB replay for seeds `288` through `319`
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
- The previous `224:255` replay established the explicit
  `--fk-graph-replay-attempts 10` budget for bounded FK graph raw-client
  deadlock recovery, and the `256:287` replay reused that budget.

## Scope And Non-Goals

In scope:

- Run one dependency-free command-plan check for seeds `288` through `319` at
  two rounds with `--fk-graph-replay-attempts 10`.
- Run one Docker-backed MariaDB 11.8 seed sweep for seeds `288` through `319`
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
  --seed-range 288:319
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
- Run the direct `288:319` check-mode seed sweep at two rounds with
  `--fk-graph-replay-attempts 10`.
- Run the Docker-backed `288:319` replay at two rounds with
  `--fk-graph-replay-attempts 10`.
- Inspect the replay manifest, final `expected.out` files, empty
  `expected.err` files, and FK graph retry directories.
- Run focused production CTest coverage for the combined seed-sweep checks and
  fixed seeded-suite checks.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Completed.

## Evidence

The direct dependency-free command-plan check passed for the replay window:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-288-319-r2-attempt10-check --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --fk-graph-replay-attempts 10 --seed-range 288:319 --check
seed=288
...
seed=319
check=ok
ownerless_external_mariadb_seed_sweep_check=ok
```

Docker-backed MariaDB 11.8 replay passed the same `288` through `319` window:

```text
tools/ownerless-external-mariadb-seed-sweep --output /tmp/mylite-ownerless-external-seed-sweep-288-319-r2-attempt10-replay --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --fk-graph-replay-attempts 10 --seed-range 288:319
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
fk_graph_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

The replay manifest recorded `image=mariadb:11.8`, `mode=replay`,
`suite_count=3`, `random_tx_rounds=2`, `ddl_rounds=2`,
`fk_graph_rounds=2`, `suite_fk_graph_replay_attempts=10`, `seed_count=32`,
and seeds `288` through `319`. All `96` random transaction, DDL stress, and
FK graph final `expected.out` files ended in `ok`, and all `96` final
`expected.err` files were empty. FK graph seeds `288`, `294`, `298`, `304`,
`308`, and `310` recovered on attempt `2` after raw external MariaDB `1213` /
SQLSTATE `40001` deadlock exits.

Focused production CTest coverage and the CI build-mode guard passed:

```text
cmake --preset prod

ctest --preset prod -R '^tools\.ownerless-external-mariadb-seed-sweep-wide-check$' --output-on-failure
tools.ownerless-external-mariadb-seed-sweep-wide-check ... Passed

ctest --preset prod -R '^tools\.ownerless-external-mariadb-seed-sweep-check$' --output-on-failure
tools.ownerless-external-mariadb-seed-sweep-check ... Passed

ctest --preset prod -R '^tools\.ownerless-(random-tx-seed-suite|ddl-stress-seed-suite|fk-graph-seed-suite)$' --output-on-failure
tools.ownerless-random-tx-seed-suite ... Passed
tools.ownerless-fk-graph-seed-suite ... Passed
tools.ownerless-ddl-stress-seed-suite ... Passed

tools/check-ci-production-builds
ci_production_build_audit_ok=.github/workflows/ci.yml
```

## Acceptance Criteria

- The direct check-mode command validates seeds `288` through `319` at two
  rounds for all three suites with an explicit FK graph retry budget.
- Docker-backed MariaDB 11.8 replay passes random transaction, DDL stress, and
  FK graph seeded suites for seeds `288` through `319` at two rounds.
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
