# Ownerless External Seed 192-223 Replay

## Problem Statement

Ownerless external stress evidence includes deterministic full trace replay,
fixed noncontiguous seed replay, dependency-free seed-sweep checks through seed
`31`, Docker-backed combined seed-sweep replay through seed `191`, and focused
seed-suite replay for random transaction, DDL stress, and foreign-key graph
inputs. Full randomized MariaDB/RQG execution remains planned, but the next
bounded evidence slice is to widen default dependency-free seed-sweep planning
and replay the next contiguous seed window through a real MariaDB 11.8 server.

This slice records Docker-backed MariaDB replay for seeds `192` through `223`
at two rounds across the random transaction, DDL stress, and foreign-key graph
seeded suites, and widens the default dependency-free seed-sweep CTest plan
from seeds `0` through `31` to seeds `0` through `63`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-external-mariadb-seed-sweep` runs the random transaction,
  DDL stress, and FK graph seeded suites through one disposable MariaDB
  container by default.
- `tools/ownerless-external-mariadb-seed-sweep --check` validates the same
  command plan without Docker or an external server.
- The wrapper caps one invocation at `64` seeds, so a `0:63` check-mode window
  is the widest single dependency-free combined seed sweep currently accepted.
- `tools/ownerless-random-tx-seed-suite`,
  `tools/ownerless-ddl-stress-seed-suite`, and
  `tools/ownerless-fk-graph-seed-suite` generate and validate per-seed SQL
  traces before external replay.
- `tools/ownerless-fk-graph-seed-suite` retries a whole seed when a raw
  external MariaDB client exits with transient `1205`/`1213` contention before
  the final oracle can run.

## Scope And Non-Goals

In scope:

- Widen `tools.ownerless-external-mariadb-seed-sweep-wide-check` to seeds `0`
  through `63` at two rounds for all three seeded suites.
- Run one dependency-free command-plan check for seeds `192` through `223` at
  two rounds.
- Run one Docker-backed MariaDB 11.8 seed sweep for seeds `192` through `223`
  at two rounds.
- Include random transaction, DDL stress, and FK graph seeded suites.
- Record manifest, final oracle, and FK graph retry evidence.
- Update compatibility and ownerless concurrency docs.

Out of scope:

- Adding a new SQL generator, RQG runner, SQLancer runner, shrinker, or default
  Docker dependency.
- Raising the seed-sweep wrapper's per-invocation seed cap.
- Changing MyLite runtime behavior, storage layout, public APIs, or directory
  lifecycle behavior.
- Claiming long-running randomized external MariaDB/RQG stress is complete.

## Design

The CTest registration now uses the widest supported check-mode range:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output <ctest-binary-dir>/ownerless-external-mariadb-seed-sweep-wide-check \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --seed-range 0:63 \
  --check
```

The next real-client evidence window uses the existing combined wrapper:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output /tmp/mylite-ownerless-external-seed-sweep-192-223-r2-replay \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --seed-range 192:223
```

The replay uses one disposable `mariadb:11.8` container and writes a
self-describing manifest plus per-suite trace and log directories. The
dependency-free direct check remains useful for fast validation, while the
Docker replay provides real-client oracle evidence.

## Compatibility Impact

No product SQL behavior changes. The slice broadens default deterministic
seed-sweep planning and external MariaDB evidence for generated random
transaction, DDL stress, and FK graph inputs while keeping full randomized
external MariaDB/RQG stress planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Check-mode artifacts are written
under the CTest build tree. Replay artifacts are written under the
caller-provided output directory, and the disposable MariaDB container is
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
- Run the widened `0:63` CTest check-mode seed sweep at two rounds.
- Run the direct `192:223` check-mode seed sweep at two rounds.
- Run the Docker-backed `192:223` replay at two rounds.
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

The widened direct dependency-free command-plan check passed for all three
suites:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-0-63-r2-check --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 0:63 --check
seed=0
...
seed=63
check=ok
ownerless_external_mariadb_seed_sweep_check=ok
```

The direct dependency-free command-plan check also passed for the replay
window:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-192-223-r2-check --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 192:223 --check
seed=192
...
seed=223
check=ok
ownerless_external_mariadb_seed_sweep_check=ok
```

After reconfiguring the production preset, focused CTest coverage passed:

```text
ctest --preset prod -R '^tools\.ownerless-external-mariadb-seed-sweep-wide-check$' --output-on-failure
tools.ownerless-external-mariadb-seed-sweep-wide-check ... Passed

ctest --preset prod -R '^tools\.ownerless-external-mariadb-seed-sweep-check$' --output-on-failure
tools.ownerless-external-mariadb-seed-sweep-check ... Passed

ctest --preset prod -R '^tools\.ownerless-(random-tx-seed-suite|ddl-stress-seed-suite|fk-graph-seed-suite)$' --output-on-failure
tools.ownerless-random-tx-seed-suite ... Passed
tools.ownerless-fk-graph-seed-suite ... Passed
tools.ownerless-ddl-stress-seed-suite ... Passed
```

Docker-backed MariaDB 11.8 replay then passed the same `192` through `223`
window:

```text
tools/ownerless-external-mariadb-seed-sweep --output /tmp/mylite-ownerless-external-seed-sweep-192-223-r2-replay --random-tx-rounds 2 --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 192:223
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
fk_graph_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

The replay manifest recorded `image=mariadb:11.8`, `mode=replay`,
`suite_count=3`, `random_tx_rounds=2`, `ddl_rounds=2`,
`fk_graph_rounds=2`, `seed_count=32`, and seeds `192` through `223`. All
`96` random transaction, DDL stress, and FK graph final `expected.out` files
ended in `ok`, and all `96` final `expected.err` files were empty. FK graph
seeds `211`, `213`, `218`, and `219` recovered on attempt `2` after raw
external MariaDB `1213` / SQLSTATE `40001` deadlock exits, matching the
suite's bounded whole-seed retry model.

## Acceptance Criteria

- The registered wide check validates seeds `0` through `63` at two rounds for
  all three seeded suites.
- The direct check-mode command validates seeds `192` through `223` at two
  rounds for all three suites.
- Docker-backed MariaDB 11.8 replay passes random transaction, DDL stress, and
FK graph seeded suites for seeds `192` through `223` at two rounds.
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
