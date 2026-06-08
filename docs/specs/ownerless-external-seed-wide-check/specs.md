# Ownerless External Seed Wide Check

## Problem Statement

Ownerless external stress evidence already includes fixed noncontiguous seeds,
a dependency-free combined seed sweep over seeds `0` through `15`, and focused
Docker-backed MariaDB replay over seeds `0` through `7`. The remaining
randomized/RQG gap is still intentionally open, but default CTest can cheaply
validate a wider deterministic generated-input plan before any
environment-owned long-running replay is attempted.

This slice adds a wider dependency-free seed-sweep CTest check and records a
bounded real MariaDB replay for the next contiguous seed window.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-external-mariadb-seed-sweep` already supports inclusive
  `--seed-range`, independent per-suite round counts, check mode, replay mode,
  and a self-describing manifest.
- `tools/ownerless-random-tx-seed-suite` and
  `tools/ownerless-ddl-stress-seed-suite` own the generated SQL and final
  oracles for each seed.
- Existing default CTest coverage registers
  `tools.ownerless-external-mariadb-seed-sweep-check` for seeds `0` through
  `15` at rounds `3`.

## Scope And Non-Goals

In scope:

- Add `tools.ownerless-external-mariadb-seed-sweep-wide-check` as a
  dependency-free CTest check over seeds `0` through `31` at two rounds for
  both seeded suites.
- Record bounded Docker-backed MariaDB replay evidence for seeds `8` through
  `15` at two rounds, complementing the existing seeds `0` through `7` replay.
- Update compatibility and ownerless cross-process docs.

Out of scope:

- Adding a new random SQL generator, RQG runner, SQLancer runner, shrinker, or
  default Docker dependency.
- Changing MyLite runtime behavior, SQL compatibility, public APIs, native
  storage layout, or directory lifecycle behavior.
- Claiming long-running randomized external MariaDB/RQG stress is complete.

## Design

Register a second seed-sweep CTest command:

```sh
tools/ownerless-external-mariadb-seed-sweep \
  --output <ctest-binary-dir>/ownerless-external-mariadb-seed-sweep-wide-check \
  --random-tx-rounds 2 \
  --ddl-rounds 2 \
  --seed-range 0:31 \
  --check
```

The existing `0:15` rounds-3 check remains in place. The new check trades
depth for breadth: it validates twice as many seeds while keeping the generated
plan cheap enough for default production CTest. Real MariaDB replay remains
opt-in and environment-owned.

## Compatibility Impact

No product compatibility behavior changes. The slice increases deterministic
external-oracle evidence for generated random transaction and DDL stress inputs
and keeps full randomized external MariaDB/RQG stress marked planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Check-mode artifacts are written
under the CTest build directory. Optional replay artifacts are written under
the requested output directory, and the disposable MariaDB container is removed
by default.

## Native Storage Impact

No MyLite native-storage format changes. Optional replay exercises unmodified
MariaDB 11.8 native InnoDB through the existing SQL trace-runner contract.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes.
Default CTest coverage uses only repository scripts and does not require
Docker.

## Verification Plan

- Run `bash -n tools/ownerless-external-mariadb-seed-sweep`.
- Run the direct wider check-mode command for seeds `0` through `31`.
- Reconfigure the production preset and run the focused CTest for the new
  wide check.
- Run the existing focused seeded suite and seed-sweep CTests.
- If Docker is available, replay seeds `8` through `15` at two rounds for both
  seeded suites and inspect final oracle logs plus expected-error stderr files.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Default CTest registers and passes
  `tools.ownerless-external-mariadb-seed-sweep-wide-check`.
- The wide check validates both seeded suites over seeds `0` through `31` at
  two rounds.
- Existing fixed-seed and seed-sweep checks still pass.
- Bounded Docker-backed replay evidence extends contiguous seed replay beyond
  the existing `0:7` window when Docker is available.
- Documentation continues to distinguish deterministic seed replay from true
  randomized external MariaDB/RQG stress.

## Evidence

The direct dependency-free command-plan check passed for both seeded suites
over seeds `0` through `31` at rounds `2`:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-0-31-r2-check-probe --random-tx-rounds 2 --ddl-rounds 2 --seed-range 0:31 --check
seed=0
...
seed=31
ownerless_external_mariadb_seed_sweep_check=ok
elapsed_seconds=1
```

Docker-backed MariaDB 11.8 replay passed for both seeded suites over seeds `8`
through `15` at rounds `2`:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/manual-external-seed-sweep-8-15-r2-replay --random-tx-rounds 2 --ddl-rounds 2 --seed-range 8:15
seed_count=8
seed=8
...
seed=15
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

The replay manifest recorded `mode=replay`, `random_tx_rounds=2`,
`ddl_rounds=2`, and the `8` through `15` seed set. All final random transaction
and DDL stress `expected.out` files ended in `ok`, and all final
`expected.err` files were empty. The disposable seed-sweep container was
removed.

After reconfiguring `prod`, the focused CTest selector passed the existing and
new seed checks:

```text
ctest --preset prod -R 'tools\.ownerless-(external-mariadb-seed-sweep(-wide)?-check|random-tx-seed-suite|ddl-stress-seed-suite)$' --output-on-failure
tools.ownerless-external-mariadb-seed-sweep-check ... Passed
tools.ownerless-external-mariadb-seed-sweep-wide-check ... Passed
tools.ownerless-random-tx-seed-suite ... Passed
tools.ownerless-ddl-stress-seed-suite ... Passed
100% tests passed, 0 tests failed out of 4
```

## Risks And Follow-Up

- A wider deterministic check can catch exporter and oracle regressions, but it
  cannot replace long-running randomized generator coverage.
- The `0:31` check is cheap locally, but if future SQL generation grows enough
  to threaten CTest timeout budgets, it should become an opt-in profile rather
  than slow default CI.
