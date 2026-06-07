# Ownerless Seed 211 External Replay

## Problem Statement

Seed `211` currently broadens deterministic random transaction and DDL stress
input only through dependency-free trace-runner checks. The random transaction
seed suite also lacks a dedicated disposable MariaDB Docker wrapper, so focused
external replay evidence for seed `211` is awkward to run and easy to omit.

This slice adds the missing random transaction wrapper and records bounded
MariaDB 11.8 replay evidence for seed `211` in both seeded suites. It does not
claim long-running randomized RQG/SQLancer coverage.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-random-tx-seed-suite` generates one deterministic random
  transaction trace per seed and replays each through
  `tools/ownerless-sql-trace-runner` when a MariaDB-compatible client is
  supplied.
- `tools/ownerless-ddl-stress-seed-suite` provides the same multi-seed wrapper
  shape for DDL stress traces.
- `tools/ownerless-external-mariadb-ddl-seed-smoke` already contains the
  disposable `mariadb:11.8` container lifecycle, readiness probe, Docker client
  argument forwarding, external manifest, and dependency-free `--check` plan
  pattern needed by the random transaction wrapper.

## Design

Add `tools/ownerless-external-mariadb-random-tx-seed-smoke`:

- Start a disposable `mariadb:11.8` Docker container.
- Wait for `mariadb-admin ping`.
- Run `tools/ownerless-random-tx-seed-suite` with the Docker-backed `mariadb`
  client, requested `--rounds`, optional repeated `--seed`, and replay logs.
- Write `external-manifest.txt` with image, container, rounds, seed list, trace
  directory, and log directory.
- Support `--check` so ordinary CTest coverage validates the command plan
  without Docker.

Register a CTest check-mode smoke for the new wrapper using seeds `0`, `17`,
`83`, and `211`. Extend the existing DDL seed external wrapper check to include
seed `211` as well. These CTest entries remain dependency-free and do not make
Docker a default CI requirement.

Run opt-in Docker-backed MariaDB 11.8 replay for seeds `0`, `17`, `83`, and
`211` at rounds `8` for both the random transaction and DDL stress seed suites,
then update the evidence docs from the resulting manifests and final oracle
logs.

## Scope

In scope:

- Random transaction external MariaDB 11.8 seed-smoke wrapper.
- CTest check-mode plan validation for random transaction and DDL external seed
  wrappers with seed `211`.
- Focused Docker replay evidence for seed `211` alongside the previous default
  seed set.
- Compatibility/spec documentation updates.

Out of scope:

- Default CI Docker replay.
- Product runtime, SQL behavior, native storage, or public API changes.
- True randomized RQG/SQLancer generation.
- New row-conflict, DDL class, or crash-recovery product coverage.

## Compatibility Impact

No MyLite SQL behavior, C API behavior, storage format, or runtime behavior
changes. The slice strengthens external compatibility evidence by proving the
existing deterministic seed suites replay through an unmodified MariaDB 11.8
server for seed `211`.

Full randomized external MariaDB/RQG stress remains planned.

## Database Directory And Lifecycle Impact

No MyLite database-directory behavior changes. The wrappers write generated
traces and logs under the requested output directory and mutate only the
disposable external MariaDB container's `app` database.

## Native Storage Impact

No MyLite native-storage format changes. External replay exercises ordinary
MariaDB native InnoDB transaction, savepoint, DDL, and DML behavior on the
container.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The new tool is a Bash wrapper around existing scripts. Docker remains
an opt-in evidence dependency; default CTest uses only `--check`.

## Test Plan

- Run `bash -n tools/ownerless-external-mariadb-random-tx-seed-smoke
  tools/ownerless-external-mariadb-ddl-seed-smoke`.
- Run both external wrapper `--check` paths with seeds `0`, `17`, `83`, and
  `211`.
- Run focused CTest coverage for the random transaction external wrapper, DDL
  external wrapper, and both seed-suite check tests.
- If Docker is available, run the random transaction and DDL external wrappers
  at rounds `8` with seeds `0`, `17`, `83`, and `211`.
- Inspect manifests and final oracle logs, including non-empty `*.err` files.
- Run `format-check`, `git diff --check`, and cleanup checks.

## Acceptance Criteria

- The random transaction external wrapper validates its command plan without
  Docker.
- CTest check-mode coverage includes seed `211` for both external seeded
  wrappers.
- Docker-backed MariaDB 11.8 replay succeeds for seeds `0`, `17`, `83`, and
  `211` at rounds `8` for both seeded suites when Docker is available.
- Final per-seed random transaction and DDL stress oracles report `ok`, and
  expected-error stderr files remain empty.
- Docs distinguish deterministic seed replay from true randomized external
  MariaDB/RQG stress.

## Evidence

The dependency-free wrapper plan checks passed for rounds `3` and seeds `0`,
`17`, `83`, and `211`:

```text
tools/ownerless-external-mariadb-random-tx-seed-smoke --output /tmp/mylite-random-tx-seed-smoke-check --rounds 3 --seed 0 --seed 17 --seed 83 --seed 211 --check
tools/ownerless-external-mariadb-ddl-seed-smoke --output /tmp/mylite-ddl-seed-smoke-check --rounds 3 --seed 0 --seed 17 --seed 83 --seed 211 --check
check=ok
```

Focused CTest coverage passed after regenerating `embedded-dev`:

```text
tools.ownerless-external-mariadb-ddl-seed-smoke-check
tools.ownerless-external-mariadb-random-tx-seed-smoke-check
tools.ownerless-random-tx-seed-suite
tools.ownerless-ddl-stress-seed-suite
```

Docker-backed MariaDB 11.8 random transaction replay passed for rounds `8` and
seeds `0`, `17`, `83`, and `211`:

```text
seed_count=4
seed=0
seed=17
seed=83
seed=211
random_tx_seed_suite_run=ok
external_random_tx_seed_smoke=ok
```

The final random transaction oracle logs reported `ok` for every seed, with
empty `expected.err` files:

```text
seed=0 observed_count=16 observed_sum=19336794 observed_versions=77 observed_weighted_sum=204322890 ownerless_random_tx_trace_check=ok
seed=17 observed_count=16 observed_sum=1328136094 observed_versions=77 observed_weighted_sum=11267818492 ownerless_random_tx_trace_check=ok
seed=83 observed_count=16 observed_sum=6410236494 observed_versions=77 observed_weighted_sum=54566507669 ownerless_random_tx_trace_check=ok
seed=211 observed_count=16 observed_sum=16266336694 observed_versions=77 observed_weighted_sum=137984903736 ownerless_random_tx_trace_check=ok
```

Docker-backed MariaDB 11.8 DDL stress replay passed for rounds `8` and seeds
`0`, `17`, `83`, and `211`:

```text
seed_count=4
seed=0
seed=17
seed=83
seed=211
ddl_stress_seed_suite_run=ok
external_ddl_stress_seed_smoke=ok
```

The final DDL stress oracle logs reported `ok` for every seed, with empty
`expected.err` files:

```text
seed=0 observed_total=158 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=17 observed_total=414 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=83 observed_total=412 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=211 observed_total=416 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
```

## Risks And Unresolved Questions

- Seeded traces broaden deterministic input schedules but are still not a true
  randomized RQG generator.
- The random transaction seed variants preserve worker-owned row partitions and
  do not add new cross-worker row-lock conflict classes.
- The DDL seed variants preserve the existing DDL lifecycle shape and do not add
  unsupported partition, special-index, external-directory, or explicit
  tablespace-management classes.
