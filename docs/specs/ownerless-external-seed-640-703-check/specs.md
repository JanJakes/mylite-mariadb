# Ownerless External Seed 640-703 Check

## Problem Statement

Ownerless external stress evidence already includes Docker-backed MariaDB 11.8
seed-sweep replay for the generated random transaction, DDL stress, and foreign
key graph suites through seed `767`. The dependency-free production CTest
command-plan gate still covers only the older `576` through `639` window.

The next bounded slice is to advance that always-available CTest gate to the
next replay-backed window, seeds `640` through `703`, without adding Docker to
default CI or claiming that long-running randomized RQG is complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-external-mariadb-seed-sweep` validates generated seed-sweep
  command plans in `--check` mode without Docker, and uses Docker only for the
  opt-in replay path.
- `tools/ownerless-external-mariadb-seed-sweep` caps each invocation at `64`
  seeds, which matches the existing seed-sweep next-check CTest shape.
- `docs/specs/ownerless-external-seed-640-671-replay/specs.md` records
  Docker-backed MariaDB 11.8 replay evidence for seeds `640` through `671`.
- `docs/specs/ownerless-external-seed-672-703-replay/specs.md` records
  Docker-backed MariaDB 11.8 replay evidence for seeds `672` through `703`.
- `docs/specs/ownerless-external-seed-736-767-replay/specs.md` advanced the
  persistent dependency-free next-check gate only through `576` through `639`,
  leaving `640` through `703` as the next replay-backed command-plan window.

## Scope And Non-Goals

In scope:

- Change `tools.ownerless-external-mariadb-seed-sweep-next-check` to validate
  seeds `640` through `703` at two rounds across random transaction, DDL
  stress, and FK graph generated suites.
- Run the direct dependency-free seed-sweep check for seeds `640` through
  `703`.
- Run the focused production CTest seed-sweep check selector.
- Update compatibility and ownerless concurrency documentation.

Out of scope:

- Running new Docker-backed MariaDB replay in this slice; the `640` through
  `703` replay artifacts are already recorded by the prior replay specs.
- Adding SQLancer, RQG, a shrinker, or a new SQL generator.
- Changing MyLite runtime behavior, storage layout, public APIs, native redo,
  page-version WAL, recovery, or directory lifecycle behavior.
- Claiming that external MariaDB/RQG stress or ownerless concurrency as a whole
  is complete.

## Design

The production CTest `tools.ownerless-external-mariadb-seed-sweep-next-check`
continues to use the existing combined wrapper in dependency-free check mode:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output <build-dir>/ownerless-external-mariadb-seed-sweep-next-check \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --fk-graph-rounds 2 \
  --seed-range 640:703 \
  --check
```

This keeps default CI independent of Docker while making the persistent check
track the next already-replayed external seed window. The check validates tool
argument expansion, per-suite command planning, seed enumeration, and manifest
metadata for all three generated suites.

## Compatibility Impact

No product SQL behavior changes. The slice broadens dependency-free generated
input evidence for existing external replay tooling and keeps the compatibility
matrix explicit that longer randomized external MariaDB/RQG stress remains
planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Check-mode output is written
under the caller-provided build output directory and does not open or mutate a
MyLite database directory.

## Native Storage Impact

No MyLite native storage format changes. The backing replay evidence exercises
unmodified MariaDB 11.8 native InnoDB through generated SQL and final oracles;
this slice only moves the dependency-free command-plan gate to that
replay-backed window.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes.
Default CTest remains dependency-free. Docker remains required only for opt-in
external replay commands.

## Verification Plan

- Run `bash -n` for the combined seed sweep and suite wrappers.
- Run the direct `640:703` check-mode seed sweep at two rounds.
- Refresh the production CTest build tree if needed.
- Run focused production CTest coverage for the combined seed-sweep checks.
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
  build/manual-external-seed-sweep-640-703-r2-check --random-tx-rounds 2
  --ddl-rounds 2 --fk-graph-rounds 2 --seed-range 640:703 --check` passed and
  ended with `ownerless_external_mariadb_seed_sweep_check=ok`.
- `cmake --preset prod` refreshed the production CTest build tree.
- `ctest --preset prod -R
  '^tools\.(ownerless-external-mariadb-seed-sweep(-wide|-next)?-check|ownerless-random-tx-seed-suite|ownerless-fk-graph-seed-suite|ownerless-ddl-stress-seed-suite)$'
  --output-on-failure` passed 6/6 tests in 4.89 seconds.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed before staging.

## Acceptance Criteria

- Production CTest includes a dependency-free combined seed-sweep check for
  seeds `640` through `703` at two rounds.
- The direct check-mode command validates seeds `640` through `703` at two
  rounds for all three suites.
- Documentation distinguishes this persistent command-plan gate from optional
  Docker-backed replay and full randomized external MariaDB/RQG stress.
- Focused production CTest and static checks pass.

## Risks And Follow-Up

- This is deterministic generated-input command-plan coverage, not full
  randomized RQG.
- Docker-backed replay through seed `767` remains bounded replay evidence, not
  a substitute for live-peer DDL/file-lifecycle recovery or broader native
  redo/checkpoint reconciliation.
- The next correctness work remains broader recovery, DDL/file-lifecycle,
  active-reader, native history-proof replacement, and external stress gaps.
