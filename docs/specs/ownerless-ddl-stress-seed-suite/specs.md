# Ownerless DDL Stress Seed Suite

## Problem Statement

The deterministic ownerless SQL trace suite has full scale-2 Docker-backed
MariaDB replay evidence, and the random transaction trace has a small seed
suite. The remaining external MariaDB/RQG gap still includes broader DDL
schedule variation. A full randomized RQG generator remains larger than one
slice, but the existing DDL stress trace can expose deterministic seed variants
that external runners can replay through the common trace runner.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-ddl-stress-trace` mirrors the product DDL/DML stress shape:
  three DDL workers repeatedly create, alter, online-index, rename, truncate,
  and drop InnoDB tables while two DML workers update a stable table and a
  reader checks aggregate monotonicity.
- `tools/ownerless-sql-trace-runner` already validates or replays generated
  trace directories through a MariaDB-compatible command-line client.
- `tools/ownerless-random-tx-seed-suite` provides the local pattern for a
  dependency-free multi-seed trace wrapper.

## Design

Add `--seed N` to `tools/ownerless-ddl-stress-trace`:

- Seed `0` preserves the original deterministic schedule and oracle values.
- Nonzero seeds keep the same worker count, table lifecycle, online-index,
  truncate/drop, DML, and reader shape.
- Nonzero seeds vary DDL row payload/default values and DML update increments,
  producing distinct final aggregate oracles while keeping all increments
  positive so the reader monotonicity check remains valid.
- `manifest.txt` records `seed=N` and
  `schedule=ownerless_ddl_stress_seeded_v1`.

Add `tools/ownerless-ddl-stress-seed-suite`:

- It accepts repeatable `--seed N`, defaulting to seeds `0`, `17`, and `83`.
- It generates one trace directory per seed under `seed-<N>/`.
- In `--check` mode it validates every generated trace through
  `ownerless-sql-trace-runner --check` without Docker or an external SQL
  server.
- In replay mode it forwards `--client`, repeated `--client-arg`, and
  `--log-dir` to the common trace runner for each seed.
- It writes `ddl-stress-seed-suite-manifest.txt` with the rounds and seed list.

The seed suite is intentionally separate from `ownerless-sql-trace-suite` so
the 11-family deterministic suite count remains stable. Seeded DDL variants
are additional external-oracle input, not a claim that long-running randomized
RQG/SQLancer coverage is complete.

The later `ownerless-seed211-trace-checks` slice keeps the default seed suite
and Docker replay evidence at `0`, `17`, and `83`, but adds seed `211` to the
dependency-free CTest check-mode invocation for additional deterministic DDL
schedule variation.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The slice broadens external compatibility evidence by making DDL stress
schedule variants reproducible and replayable through the existing
MariaDB-compatible trace runner.

## Database Directory And Lifecycle Impact

No MyLite database-directory changes. Generated traces and replay logs live
under caller-provided output directories; replay mutates only the target SQL
server's `app` schema.

## Native Storage Impact

No MyLite native storage format changes. External replay continues to exercise
ordinary InnoDB create, alter, online-index, rename, truncate, drop, and DML
paths on the replay target.

## Public API, Build, Size, And Dependencies

No public API, production build, binary-size, license, or dependency changes.
The new tool is a dependency-free Bash script used by CTest and optional
external replay.

## Test Plan

- Run `bash -n tools/ownerless-ddl-stress-trace
  tools/ownerless-ddl-stress-seed-suite`.
- Run seed-0 and nonzero seed exporter checks.
- Run `tools/ownerless-ddl-stress-seed-suite --rounds 3 --seed 0 --seed 17
  --seed 83 --seed 211 --check`.
- Run focused CTest coverage for `tools.ownerless-ddl-stress-trace`,
  `tools.ownerless-ddl-stress-seed-suite`, and scaled trace-suite checks.
- Run `tools/ownerless-external-mariadb-ddl-seed-smoke --rounds 8 --seed 0
  --seed 17 --seed 83` when Docker is available.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- Seed `0` keeps the existing round-3 DDL stress oracle values.
- Nonzero seeds produce distinct deterministic DDL stress oracle values.
- The seed suite validates every requested seed through the common trace-runner
  plan.
- Optional external MariaDB replay uses the same generated seed directories and
  final aggregate oracles.
- Docs continue to mark long-running randomized external MariaDB/RQG stress as
  planned.

## Evidence

Dependency-free check-mode validation passed for rounds `3` and seeds `0`,
`17`, `83`, and, in the later seed-211 follow-up, `211`. Seed `0` preserved
the existing round-3 total, while nonzero seeds produced distinct
deterministic totals:

```text
seed=0 expected_total=78
seed=17 expected_total=174
seed=83 expected_total=172
seed=211 expected_total=176
ddl_stress_seed_suite_check=ok
```

Focused tool CTests passed:

```text
tools.ownerless-ddl-stress-trace
tools.ownerless-ddl-stress-seed-suite
tools.ownerless-sql-trace-suite
tools.ownerless-sql-trace-suite-full-scaled
```

Focused Docker-backed MariaDB 11.8 replay remains recorded for rounds `8` and
seeds `0`, `17`, and `83`:

```text
seed=0 observed_total=158 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=17 observed_total=414 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
seed=83 observed_total=412 observed_stress_tables=0 ownerless_ddl_stress_trace_check=ok
ddl_stress_seed_suite_run=ok
external_ddl_stress_seed_smoke=ok
```

All final `expected.err` files were empty.

## Risks And Unresolved Questions

- Seeded traces broaden deterministic input schedules but are still not a true
  randomized RQG generator.
- The seeded variants preserve the product DDL stress table lifecycle and worker
  counts, so they do not add unsupported partition, special-index, external
  directory, or tablespace detach/import classes.
