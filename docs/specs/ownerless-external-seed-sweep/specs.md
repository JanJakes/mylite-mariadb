# Ownerless External Seed Sweep

## Problem Statement

The ownerless random transaction and DDL stress seed suites can replay selected
deterministic seeds through MariaDB 11.8, but each external wrapper currently
runs one suite at a time. That is enough for fixed evidence like seeds `0`,
`17`, `83`, and `211`, but it makes exploratory contiguous seed sweeps awkward:
callers must start separate disposable MariaDB containers or manually thread
the same Docker client invocation through both seed-suite tools.

This slice adds one opt-in external seed-sweep wrapper that runs the existing
seed suites over an explicit seed list or inclusive seed range through one
disposable MariaDB 11.8 server. It does not claim true randomized
RQG/SQLancer coverage.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-random-tx-seed-suite` generates one deterministic random
  transaction trace per seed and replays each trace through
  `tools/ownerless-sql-trace-runner` when a MariaDB-compatible client is
  supplied. Seed `0` mirrors the product C stress schedule; nonzero seeds are
  deterministic external-oracle variants.
- `tools/ownerless-ddl-stress-seed-suite` provides the same multi-seed pattern
  for DDL stress traces over create, alter, online-index, rename, truncate,
  drop, and DML schedules.
- `tools/ownerless-external-mariadb-random-tx-seed-smoke` and
  `tools/ownerless-external-mariadb-ddl-seed-smoke` contain the disposable
  Docker lifecycle, readiness probe, `--check` plan, seed validation, and
  replay-log layout already used for focused external evidence.

## Design

Add `tools/ownerless-external-mariadb-seed-sweep`:

- Accept repeated `--suite random-tx` and `--suite ddl`, defaulting to both.
- Accept repeated `--seed` and repeated inclusive `--seed-range START:END`,
  defaulting to the current fixed evidence set `0`, `17`, `83`, and `211`.
- Start one disposable `mariadb:11.8` container, wait for `mariadb-admin ping`,
  and replay the selected seed suites through a Docker-backed `mariadb` client.
- Write `external-seed-sweep-manifest.txt` with the image, container name,
  suite list, seed list, and per-suite round counts.
- Support `--check`, which validates the expanded seed/suite command plan by
  calling the existing external seed wrappers in check mode without Docker.
- Build repeated seed arguments with ordinary Bash arrays rather than nameref
  helpers so the dependency-free check path also runs on macOS bash 3.2.

The wrapper delegates all SQL generation and oracle execution to the existing
seed suites and trace runner. It does not duplicate stress formulas or SQL
oracles.

## Scope

In scope:

- One combined external MariaDB 11.8 seed-sweep wrapper.
- CTest check-mode coverage for the wrapper over a contiguous seed range.
- Focused Docker-backed MariaDB replay evidence for both seeded suites over
  seeds `0` through `7` at rounds `4`.
- Compatibility/spec documentation updates that describe this as deterministic
  generated-input evidence.

Out of scope:

- Default CI Docker replay.
- Full randomized RQG/SQLancer generation or shrinking.
- New MyLite product runtime behavior, SQL behavior, storage format, or public
  API behavior.
- Unsupported DDL classes such as partitions, external table directories,
  special indexes, explicit tablespace detach/import, compression/encryption
  storage options, or table admin commands.

## Compatibility Impact

No MyLite runtime or SQL compatibility behavior changes. The slice strengthens
external compatibility evidence by making broader deterministic seed replay
practical and reproducible against an unmodified MariaDB 11.8 server.

Full randomized external MariaDB/RQG stress remains planned and
environment-owned.

## Database Directory And Lifecycle Impact

No MyLite database-directory behavior changes. The wrapper writes generated
trace packages, replay logs, and a manifest under the requested output
directory. The disposable external MariaDB container mutates only its `app`
database and is removed by default.

## Native Storage Impact

No MyLite native-storage format changes. External replay exercises MariaDB
native InnoDB transaction, savepoint, DDL, DML, and contention behavior through
the existing trace-runner contract.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. Docker remains an opt-in evidence dependency; default CTest only uses
the dependency-free `--check` path.

## Test Plan

- Run `bash -n tools/ownerless-external-mariadb-seed-sweep`.
- Run `tools/ownerless-external-mariadb-seed-sweep --output ... --rounds 4
  --seed-range 0:5 --check`.
- Run focused CTest coverage for
  `tools.ownerless-external-mariadb-seed-sweep-check`.
- Run single-suite `--check` probes for `random-tx` and `ddl` with an explicit
  seed to cover seed-argument forwarding without Docker.
- If Docker is available, run
  `tools/ownerless-external-mariadb-seed-sweep --output ... --rounds 4
  --seed-range 0:7`.
- Inspect manifests and final oracle logs, and confirm expected-error stderr
  files remain empty.
- Run `format-check`, `git diff --check`, and cleanup checks.

## Acceptance Criteria

- The seed sweep validates explicit seed ranges and rejects invalid or duplicate
  seeds before Docker startup.
- The dependency-free CTest check path covers a contiguous seed range for both
  seeded suites.
- The check path stays compatible with macOS bash 3.2.
- Docker-backed MariaDB 11.8 replay succeeds for both random transaction and
  DDL stress suites over seeds `0` through `7` at rounds `4`.
- Final per-seed random transaction and DDL stress oracles report `ok`, and
  expected-error stderr files remain empty.
- Docs continue to mark true randomized external MariaDB/RQG stress as planned.

## Evidence

The dependency-free command-plan check passed for both seeded suites over
seeds `0` through `5` at rounds `4`:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/ownerless-external-seed-sweep-check-0-5 --rounds 4 --seed-range 0:5 --check
seed=0
seed=1
seed=2
seed=3
seed=4
seed=5
ownerless_external_mariadb_seed_sweep_check=ok
```

The wrapper rejected duplicate seeds and reversed seed ranges before Docker
startup:

```text
duplicate_seed_rejected=ok
Duplicate seed: 1
reversed_seed_range_rejected=ok
--seed-range start must be <= end: 3:1
```

Docker-backed MariaDB 11.8 replay passed for both seeded suites over seeds
`0` through `7` at rounds `4`:

```text
seed_count=8
seed=0
seed=1
seed=2
seed=3
seed=4
seed=5
seed=6
seed=7
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

The final random transaction oracle logs reported `ok` for every seed, with
empty `expected.err` files:

```text
seed=0 observed_count=16 observed_sum=9210958 observed_versions=39 observed_weighted_sum=92988771 ownerless_random_tx_trace_check=ok
seed=1 observed_count=16 observed_sum=48810258 observed_versions=39 observed_weighted_sum=439590633 ownerless_random_tx_trace_check=ok
seed=2 observed_count=16 observed_sum=87810758 observed_versions=39 observed_weighted_sum=769094289 ownerless_random_tx_trace_check=ok
seed=3 observed_count=16 observed_sum=124110036 observed_versions=38 observed_weighted_sum=1148594286 ownerless_random_tx_trace_check=ok
seed=4 observed_count=16 observed_sum=162110636 observed_versions=38 observed_weighted_sum=1499195319 ownerless_random_tx_trace_check=ok
seed=5 observed_count=16 observed_sum=215711302 observed_versions=41 observed_weighted_sum=1955002214 ownerless_random_tx_trace_check=ok
seed=6 observed_count=16 observed_sum=237510336 observed_versions=38 observed_weighted_sum=2028983569 ownerless_random_tx_trace_check=ok
seed=7 observed_count=16 observed_sum=296811002 observed_versions=41 observed_weighted_sum=2415188289 ownerless_random_tx_trace_check=ok
```

The final DDL stress oracle logs reported `ok` for every seed, with empty
`expected.err` files:

```text
seed=0 observed_total=94 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=1 observed_total=221 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=2 observed_total=225 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=3 observed_total=219 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=4 observed_total=223 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=5 observed_total=222 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=6 observed_total=221 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=7 observed_total=225 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
```

## Risks And Unresolved Questions

- A contiguous seed sweep broadens deterministic inputs but still cannot
  replace long-running randomized RQG/SQLancer execution.
- The random transaction seed suite continues to use worker-owned row
  partitions; it does not add new cross-worker row-conflict classes.
- The DDL seed suite continues to use supported ownerless DDL classes; it does
  not make rejected or unproven DDL classes supported.
