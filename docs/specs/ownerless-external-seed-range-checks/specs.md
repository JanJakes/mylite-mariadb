# Ownerless External Seed Range Checks

## Problem

Ownerless external stress evidence includes deterministic random transaction
and DDL seed suites plus an opt-in Docker-backed seed sweep. The default CTest
coverage for the combined seed-sweep wrapper still validates only a small
contiguous seed range, and the sweep manifest does not say whether the run was
a dependency-free command-plan check or a Docker replay. That makes CI artifacts
less useful when comparing seed coverage and timing across branches.

Broader deterministic seed-range check coverage is not full randomized
MariaDB/RQG stress, but it reduces the gap by validating more generated
external-oracle inputs in every default embedded test run.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-random-tx-seed-suite` validates one deterministic random
  transaction trace per seed through `tools/ownerless-sql-trace-runner` in
  check mode, or replays it through a MariaDB-compatible SQL client.
- `tools/ownerless-ddl-stress-seed-suite` uses the same multi-seed pattern for
  deterministic DDL stress traces.
- `tools/ownerless-external-mariadb-seed-sweep` expands repeated `--seed` and
  inclusive `--seed-range START:END` arguments, validates duplicates and range
  direction before Docker startup, and delegates check-mode validation to the
  focused external seed wrappers.
- `tools/CMakeLists.txt` registers
  `tools.ownerless-external-mariadb-seed-sweep-check` as dependency-free CTest
  coverage for the combined wrapper.

## Scope And Non-Goals

- Expand the registered dependency-free seed-sweep CTest check to cover seeds
  `0` through `15` for both random transaction and DDL seeded suites.
- Keep the CTest check bounded by using three rounds per suite.
- Extend the combined seed-sweep manifest with the run mode, keep-container
  policy, per-suite round counts, and per-suite output/log paths.
- Update compatibility and ownerless cross-process docs to identify the
  broader check-mode seed range.
- Do not add a new random SQL generator, shrinker, RQG/SQLancer runner, or
  default Docker dependency.
- Do not change MyLite runtime behavior, public API behavior, storage layout,
  or SQL compatibility.

## Design

Change `tools.ownerless-external-mariadb-seed-sweep-check` from a small
rounds-4 range over seeds `0` through `5` to a wider rounds-3 range over seeds
`0` through `15`. This validates 16 random transaction traces and 16 DDL stress
traces in check mode while preserving a bounded CTest runtime.

Extend `external-seed-sweep-manifest.txt` so generated artifacts are
self-describing:

- `mode=check` or `mode=replay`,
- `keep_container=0|1`,
- per-suite round counts,
- check-mode output directories,
- replay trace/log directories.

The wrapper still delegates SQL and oracle generation to the existing seed
suites. The manifest change is informational and does not alter replay
semantics.

## Compatibility Impact

No SQL compatibility surface changes. The slice strengthens deterministic
external-oracle evidence for ownerless stress inputs and keeps true randomized
external MariaDB/RQG stress explicitly planned.

## Database Directory And Lifecycle Impact

No MyLite database-directory behavior changes. The seed-sweep wrapper writes
only generated trace/check artifacts under the requested output directory and
uses a disposable external MariaDB container only when not in `--check` mode.

## Native Storage Impact

No native storage format changes. Check mode validates generated SQL plans and
oracles; optional replay continues to exercise unmodified MariaDB native InnoDB
through the existing trace-runner contract.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. Default CTest remains dependency-free and does not require Docker.

## Test Plan

- Run `bash -n tools/ownerless-external-mariadb-seed-sweep`.
- Run `tools/ownerless-external-mariadb-seed-sweep --output ... \
  --random-tx-rounds 3 --ddl-rounds 3 --seed-range 0:15 --check`.
- Inspect `external-seed-sweep-manifest.txt` for mode, per-suite rounds, and
  output paths.
- Run focused CTest coverage for
  `tools.ownerless-external-mariadb-seed-sweep-check`.
- Run the focused random transaction and DDL seed-suite CTests that still cover
  fixed noncontiguous seeds.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The combined seed-sweep CTest check validates both seeded suites over seeds
  `0` through `15` at rounds `3`.
- The seed-sweep manifest identifies check versus replay mode and records
  per-suite round/output metadata.
- Existing fixed-seed random transaction and DDL seed-suite checks continue to
  pass.
- Documentation accurately distinguishes this deterministic seed-range check
  from true randomized external MariaDB/RQG stress.

## Evidence

The direct dependency-free command-plan check passed for both seeded suites
over seeds `0` through `15` at rounds `3`:

```text
tools/ownerless-external-mariadb-seed-sweep --output build/ownerless-external-seed-range-check-manual --random-tx-rounds 3 --ddl-rounds 3 --seed-range 0:15 --check
seed=0
...
seed=15
ownerless_external_mariadb_seed_sweep_check=ok
```

After reconfiguring `embedded-dev`, CTest registered the same command and the
focused CTest check passed:

```text
ctest --preset embedded-dev -N -R '^tools\.ownerless-external-mariadb-seed-sweep-check$' -V
Test command: ... --random-tx-rounds 3 --ddl-rounds 3 --seed-range 0:15 --check

ctest --preset embedded-dev -R '^tools\.ownerless-external-mariadb-seed-sweep-check$' --output-on-failure
tools.ownerless-external-mariadb-seed-sweep-check ... Passed
```

The generated check-mode manifest included the widened seed range and
self-describing mode/output metadata:

```text
mode=check
random_tx_rounds=3
ddl_rounds=3
suite_random_tx_output=.../random-tx-check
suite_ddl_output=.../ddl-check
seed_count=16
seed=0
...
seed=15
```

Because the local `mariadb:11.8` Docker image was already present, a small
optional replay also passed for seeds `8` and `9` at one round per suite:

```text
tools/ownerless-external-mariadb-seed-sweep --output /tmp/mylite-ownerless-seed-range-replay --random-tx-rounds 1 --ddl-rounds 1 --seed-range 8:9
random_tx_seed_suite_run=ok
ddl_stress_seed_suite_run=ok
ownerless_external_mariadb_seed_sweep=ok
```

That replay generated a `mode=replay` manifest with per-suite trace/log output
paths, all final `expected.err` files were empty, and each final
`expected.out` ended in `ok`.

## Risks And Follow-Up

- A wider deterministic seed range can expose exporter or oracle bugs, but it
  does not replace long-running randomized RQG/SQLancer execution.
- Check-mode validation does not prove Docker/client replay for the new wider
  range; optional external replay remains environment-owned evidence.
