# Ownerless Foreign-Key Graph Trace Export

## Problem

Ownerless foreign-key graph stress now covers concurrent `CASCADE`, `SET NULL`,
and `RESTRICT` workers with deterministic final oracles inside the MyLite test
harness. The remaining Phase 12 validation gap still calls out external
MariaDB/RQG-style execution. A full external runner is larger than one slice,
but the existing deterministic graph schedule can be exported as SQL traces and
an aggregate oracle so external harnesses can replay the same workload against
MariaDB-compatible targets.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc:991-1813` implements InnoDB
  foreign-key action checks for referenced-row and missing-parent cases.
- `mariadb/storage/innobase/handler/ha_innodb.cc:2122-2126` maps
  `DB_NO_REFERENCED_ROW` and `DB_ROW_IS_REFERENCED` to the handler errors used
  by SQL diagnostics.
- `mariadb/sql/handler.cc:5001-5010` maps those handler errors to MariaDB
  foreign-key SQL errors.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_foreign_key_graph_stress()` creates the deterministic graph
  schema and starts three ownerless workers.
- The same file's `run_ownerless_fk_graph_stress_worker()`,
  `ownerless_fk_graph_stress_initial_id()`,
  `ownerless_fk_graph_stress_delta()`, and
  `assert_ownerless_fk_graph_stress_state()` define the pure schedule and final
  aggregate oracle.

## Scope And Non-Goals

In scope:

- Add a repository tool that writes SQL traces matching the deterministic
  ownerless foreign-key graph stress schedule.
- Emit schema SQL, one SQL file per worker, one expected-error probe file per
  worker, expected aggregate SQL, and a manifest.
- Add a CTest smoke check that generates a small trace and validates expected
  oracle fields.
- Document that this is external-oracle scaffolding, not completion of the
  external MariaDB/RQG stress requirement.

Out of scope:

- Starting or managing an external MariaDB server.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or the in-process FK graph stress.
- Implementing retry logic inside plain SQL files; external harnesses remain
  responsible for retrying 1205/1213 transaction failures when they choose to
  run workers concurrently.

## Design

`tools/ownerless-fk-graph-trace` is a dependency-free shell tool. It accepts
`--output DIR`, `--rounds N`, and `--check`. It writes:

- `schema.sql`, which creates `app.ownerless_fk_graph_root` plus cascade,
  set-null, and restrict child tables with the same initial rows as the C
  stress test,
- `worker-1.sql` through `worker-3.sql`, each containing that worker's
  deterministic transaction schedule and final set-null parent delete,
- `negative-worker-1.sql` through `negative-worker-3.sql`, each containing
  expected foreign-key error probes annotated with expected MariaDB errno 1451
  or 1452,
- `expected.sql`, which reports observed aggregate state and returns `ok` only
  when the root rows, child rows, versions, references, and foreign-key metadata
  match the deterministic oracle,
- `manifest.txt`, which records constants and expected totals.

External harnesses can run `schema.sql`, execute worker files concurrently or
under a scheduler, then run `expected.sql`. They can run the negative probe
files under error-aware execution that asserts the annotated errno. The CTest
smoke path uses `--check` to validate generation without requiring an external
server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior changes.
The generated trace makes the existing deterministic ownerless foreign-key graph
stress portable to external MariaDB/RQG-style validation.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL intentionally
uses ordinary MariaDB DDL and DML so an external runner can create its own test
schema.

## Native Storage Impact

No native storage runtime changes. The trace covers ordinary InnoDB
foreign-key metadata and action semantics when replayed externally.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds a shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-fk-graph-trace --rounds 3 --output <tmp> --check`.
- Run `ctest --preset embedded-dev -R tools.ownerless-fk-graph-trace
  --output-on-failure`.
- Run focused ownerless FK graph stress to prove the source schedule still
  passes.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, worker, negative-probe, expected, and
  manifest files.
- The smoke test verifies deterministic aggregate oracle values are emitted.
- Documentation and compatibility notes mark FK graph trace export as covered
  while full external MariaDB/RQG execution remains planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas; future
  changes to those formulas must update the tool in the same slice.
- The generated files are inputs to external stress harnesses, not a replacement
  for actually running external MariaDB/RQG workloads.
