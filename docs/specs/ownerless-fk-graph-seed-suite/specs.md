# Ownerless FK Graph Seed Suite

## Problem

Ownerless stress already covers one deterministic foreign-key graph schedule and
exports that schedule for external MariaDB replay. The broader ownerless
completion objective still needs more external-oracle evidence before the
project can defend cross-process concurrency claims. Random transaction and DDL
stress have seeded trace suites and seed-sweep wrappers; the FK graph trace
still has only one schedule.

This slice adds deterministic seeded FK graph trace generation and wires it into
the existing dependency-free and optional Docker-backed external seed-sweep
paths. It is not full randomized RQG, but it broadens the generated external
oracle space for the FK graph surface.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc` checks foreign-key constraints and
  invokes cascade processing for referenced-row updates/deletes.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` performs cascaded delete, set-null, and
  update work for foreign-key actions.
- `mariadb/storage/innobase/row/row0upd.cc` routes update/delete execution
  through foreign-key checks and action handling.
- `mariadb/storage/innobase/include/db0err.h` defines
  `DB_NO_REFERENCED_ROW` and `DB_ROW_IS_REFERENCED`, which surface as the
  MariaDB `1452` and `1451` expected-error probes used by the trace.
- Existing MyLite trace infrastructure lives in `tools/ownerless-sql-trace-runner`,
  `tools/ownerless-random-tx-seed-suite`, `tools/ownerless-ddl-stress-seed-suite`,
  and `tools/ownerless-external-mariadb-seed-sweep`.

## Design

- Extend `tools/ownerless-fk-graph-trace` with `--seed`.
- Preserve seed `0` as the existing deterministic FK graph formula so historical
  trace-suite evidence remains comparable.
- Use nonzero seeds to vary update deltas while preserving the graph shape:
  three workers, `CASCADE`, `SET NULL`, and `RESTRICT` edges, whole-round
  retry procedures, and `1451`/`1452` negative probes.
- Add `tools/ownerless-fk-graph-seed-suite` to generate and validate or replay
  multiple seeded FK graph trace directories through the common trace runner,
  with bounded whole-seed replay attempts so raw external-client `1205`/`1213`
  exits do not hide a recoverable generated-input schedule.
- Add `tools/ownerless-external-mariadb-fk-graph-seed-smoke` as an opt-in
  disposable MariaDB wrapper for real-client replay.
- Extend `tools/ownerless-external-mariadb-seed-sweep` so the combined sweep can
  include random transaction, DDL, and FK graph seeded suites in one disposable
  external MariaDB server.

## Compatibility Impact

No product SQL behavior changes. This slice adds external-oracle inputs for the
existing ownerless FK graph compatibility surface.

## Database Directory And Lifecycle Impact

No MyLite database-directory format or lifecycle behavior changes. Generated
trace artifacts are explicit test outputs under caller-provided directories.

## Native Storage Impact

No native storage implementation changes. The generated traces exercise native
InnoDB foreign-key referential actions and expected error paths through an
external MariaDB-compatible client when replay mode is used.

## Build And Performance Impact

No production build or binary-size impact. CMake adds dependency-free check-mode
tests for the new seed suite and external wrapper command plans. Optional Docker
replay remains environment-owned and is not required for default CI.

## Test Plan

- Run `tools/ownerless-fk-graph-trace --seed 0 --check` and a nonzero seed.
- Run `tools/ownerless-fk-graph-seed-suite --seed 0 --seed 17 --seed 83
  --seed 211 --check`.
- Run `tools/ownerless-external-mariadb-fk-graph-seed-smoke --check`.
- Run `tools/ownerless-external-mariadb-seed-sweep --suite fk-graph
  --seed-range 0:15 --check`.
- Run registered CMake tool tests for the FK graph trace/seed suite and external
  seed-sweep checks.
- When Docker is available, run the FK graph seed smoke against MariaDB 11.8.
  The first bounded replay evidence covered seeds `0`, `17`, `83`, and `211` at
  rounds `2`; seed `17` recovered on whole-seed attempt `2` and seed `83`
  recovered on attempt `3` after transient raw MariaDB `1213` exits.

## Acceptance Criteria

- Seed `0` remains valid and preserves the default graph schedule.
- Nonzero seeds produce distinct manifest/oracle values and validate through the
  trace runner.
- The combined external seed sweep records FK graph suite rounds, output paths,
  and seed lists in its manifest.
- Default dependency-free CMake checks cover the new FK graph seed-suite command
  plans.
- Optional Docker-backed replay can run the same seed set through a disposable
  MariaDB server without changing the dependency-free CI default.

## Risks And Follow-Up

- This is deterministic generated-input coverage, not full randomized RQG.
- Nonzero seeds currently vary update deltas, not graph topology or statement
  ordering. Broader topology mutation and long-running external FK graph RQG
  remain planned.
- Deeper intra-action FK graph crash fuzzing remains separate hook-build work.
