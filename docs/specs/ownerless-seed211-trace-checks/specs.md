# Ownerless Seed 211 Trace Checks

## Summary

Broaden deterministic external-oracle input coverage by adding seed `211` to
the dependency-free CTest checks for the random transaction and DDL stress seed
suites.

## Problem

The existing random transaction and DDL stress seed suites default to seeds
`0`, `17`, and `83`, with focused Docker-backed MariaDB replay evidence for
that default set. The remaining external MariaDB/RQG gap still includes broader
schedule variation. A full randomized generator remains larger than one slice,
but CTest can cheaply validate one more deterministic generated seed through
the existing trace-runner check path.

## Source References

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-random-tx-trace` generates deterministic transaction and
  savepoint schedules for a given seed and emits final aggregate oracles.
- `tools/ownerless-ddl-stress-trace` generates deterministic DDL/DML lifecycle
  schedules for a given seed and emits final metadata/value oracles.
- `tools/ownerless-sql-trace-runner --check` validates generated trace
  manifests, SQL files, expected-error probes, and oracle plans without
  requiring an external SQL server.

## Scope And Non-Goals

In scope:

- Add `--seed 211` to the `tools.ownerless-random-tx-seed-suite` CTest
  registration.
- Add `--seed 211` to the `tools.ownerless-ddl-stress-seed-suite` CTest
  registration.
- Keep the seed-suite tool defaults unchanged at `0`, `17`, and `83` so
  existing external replay evidence remains precise.

Out of scope:

- Running Docker-backed MariaDB replay for seed `211` in this slice.
- Adding a true randomized RQG/SQLancer generator.
- Changing ownerless product runtime behavior.

## Design

The CTest smoke invocations already pass explicit seeds. Extend those explicit
lists from `0`, `17`, `83` to `0`, `17`, `83`, `211`. Check-mode remains
dependency-free and routes every generated seed through the common
`ownerless-sql-trace-runner --check` path.

## Compatibility Impact

No MyLite SQL behavior, C API behavior, storage format, or runtime behavior
changes. This is external deterministic trace-input coverage only. Docs must
continue to mark full long-running randomized external MariaDB/RQG stress as
planned.

## Database Directory And Lifecycle Impact

No MyLite database-directory changes. Generated traces are written under the
CMake binary tree for check-mode validation.

## Native Storage Impact

No native storage changes.

## Public API, Build, Size, And Dependencies

No public API, production build, binary-size, license, or dependency changes.
The slice adds no tools and no external dependencies.

## Test Plan

- Run `ctest --preset ownerless-stress -R
  '^tools\.ownerless-random-tx-seed-suite$' --output-on-failure`.
- Run `ctest --preset ownerless-stress -R
  '^tools\.ownerless-ddl-stress-seed-suite$' --output-on-failure`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Random transaction seed-suite CTest reports seeds `0`, `17`, `83`, and `211`
  with `random_tx_seed_suite_check=ok`.
- DDL stress seed-suite CTest reports seeds `0`, `17`, `83`, and `211` with
  `ddl_stress_seed_suite_check=ok`.
- Compatibility docs distinguish dependency-free seed-211 check coverage from
  existing Docker-backed MariaDB replay evidence for seeds `0`, `17`, and `83`.

## Risks And Follow-Up

- Seed `211` is another deterministic generated input, not full randomized
  external stress.
- Docker-backed MariaDB replay for seed `211` was completed by the later
  `ownerless-seed211-external-replay` slice.
