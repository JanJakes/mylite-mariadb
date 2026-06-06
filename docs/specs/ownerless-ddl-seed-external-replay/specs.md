# Ownerless DDL Seed External Replay

## Problem Statement

The DDL stress seed suite makes deterministic DDL schedule variants
reproducible, but its first committed coverage was dependency-free check mode.
The remaining bounded evidence gap is proving the generated seed suite can run
through a real MariaDB 11.8 client/server using the same trace runner path.

This slice adds an opt-in Docker smoke wrapper and records focused replay
evidence for the default seed set without claiming long-running randomized
RQG/SQLancer coverage.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-ddl-stress-seed-suite` generates per-seed DDL stress traces
  and replays them through `tools/ownerless-sql-trace-runner` when a caller
  supplies a MariaDB-compatible client.
- `tools/ownerless-external-mariadb-trace-smoke` already provides the Docker
  lifecycle pattern for disposable MariaDB 11.8 replay evidence.

## Design

Add `tools/ownerless-external-mariadb-ddl-seed-smoke`:

- Start a disposable `mariadb:11.8` Docker container.
- Wait for `mariadb-admin ping`.
- Run `tools/ownerless-ddl-stress-seed-suite` with the Docker-backed
  `mariadb` client, `--rounds`, optional repeated `--seed`, and replay logs.
- Write `external-manifest.txt` with image, container, rounds, seed list, trace
  directory, and log directory.
- Support `--check` for dependency-free CTest coverage of the command plan.

Register a CTest check-mode smoke so ordinary embedded test runs validate the
wrapper interface without requiring Docker.

## Scope

In scope:

- External MariaDB 11.8 replay wrapper for the seeded DDL stress suite.
- CTest check-mode plan validation.
- Documentation and compatibility evidence updates.

Out of scope:

- Default CI Docker replay.
- New product runtime behavior.
- Full randomized RQG/SQLancer generation.
- Unsupported DDL classes such as partitions, external table directories,
  storage-option tablespaces, and special indexes.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The slice strengthens external compatibility evidence by replaying
seeded DDL stress schedules through MariaDB 11.8.

Full randomized external MariaDB/RQG stress remains planned.

## Database Directory And Lifecycle Impact

No MyLite database-directory behavior changes. The wrapper writes traces and
logs under the requested output directory and mutates only the disposable
external MariaDB container's `app` database.

## Native Storage Impact

No MyLite native-storage format changes. External replay exercises MariaDB
native InnoDB create, alter, online-index, rename, truncate, drop, and DML
paths.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. Docker remains opt-in for evidence runs; default CTest only uses the
dependency-free `--check` mode.

## Test Plan

- Run `bash -n tools/ownerless-external-mariadb-ddl-seed-smoke`.
- Run the wrapper `--check` path.
- Run `ctest --preset embedded-dev -R
  'tools\\.ownerless-external-mariadb-ddl-seed-smoke-check|tools\\.ownerless-ddl-stress-seed-suite'`.
- If Docker is available, run
  `tools/ownerless-external-mariadb-ddl-seed-smoke --rounds 8 --seed 0 --seed
  17 --seed 83`.
- Inspect manifests and final expected logs.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- The wrapper validates its command plan without Docker.
- The CTest check-mode smoke is registered.
- Docker-backed MariaDB 11.8 replay succeeds for seeds `0`, `17`, and `83` at
  rounds `8` when Docker is available.
- Final per-seed DDL stress oracles report `ok`.
- Docs keep full randomized external MariaDB/RQG stress marked as planned.

## Evidence

The dependency-free wrapper plan check passed for rounds `3` and seeds `0`,
`17`, and `83`:

```text
rounds=3
seed=0
seed=17
seed=83
check=ok
```

Focused Docker-backed MariaDB 11.8 replay passed for rounds `8` and seeds `0`,
`17`, and `83`:

```text
seed_count=3
seed=0
seed=17
seed=83
ddl_stress_seed_suite_run=ok
external_ddl_stress_seed_smoke=ok
```

The final MariaDB oracle logs reported `ok` for every seed, with empty
`expected.err` files:

```text
seed=0 observed_total=158 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=17 observed_total=414 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=83 observed_total=412 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
```

## Risks And Unresolved Questions

- Seeded DDL traces broaden deterministic external replay evidence but are
  still not a true randomized RQG generator.
- Docker image availability and host load can affect wall time, so replay
  evidence records trace success and final oracle output rather than product
  performance.
