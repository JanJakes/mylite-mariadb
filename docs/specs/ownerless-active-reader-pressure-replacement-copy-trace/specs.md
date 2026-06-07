# Ownerless Active-Reader Pressure Replacement Copy Trace

## Problem

Internal ownerless SQL coverage now proves `CREATE OR REPLACE TABLE ... LIKE`
and `CREATE OR REPLACE TABLE ... AS SELECT` are throttled under active-reader
retained-WAL pressure. The deterministic external active-reader pressure trace
should emit the same replacement-copy SQL family so MariaDB/RQG-style harnesses
can replay it alongside the existing snapshot reader and large-row worker.

This slice extends the existing trace exporter only; it does not change
ownerless runtime behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc` implements `CREATE TABLE ... LIKE`,
  `CREATE TABLE ... SELECT`, and `CREATE OR REPLACE TABLE` replacement paths.
- `tools/ownerless-active-reader-pressure-trace` already generates a portable
  trace package with `schema.sql`, `worker-1.sql`, `reader.sql`,
  `expected.sql`, and `manifest.txt`.
- `tools/ownerless-sql-trace-runner` checks that generated trace packages have
  non-empty schema, concurrent worker/reader, expected oracle, and manifest
  files before an external client replay is attempted.

## Design

Extend `tools/ownerless-active-reader-pressure-trace`:

1. Create old replacement targets and source tables for both `LIKE` and CTAS
   replacement-copy paths during `schema.sql`.
2. Have the worker procedure execute `CREATE OR REPLACE TABLE ... LIKE`, insert
   copied rows through the copied secondary index shape, and execute
   `CREATE OR REPLACE TABLE ... AS SELECT` after the deterministic large-row
   update rounds.
3. Add worker and final expected oracles for replacement-copy row counts, value
   sums, copied-column metadata, copied secondary-index metadata, and old-index
   or old-column absence.
4. Record replacement-copy expected sums in `manifest.txt`.
5. Extend `--check` to grep for the replacement-copy SQL and final oracle
   names.

## Scope And Non-Goals

In scope:

- Deterministic external trace input for replacement-copy DDL during the
  active-reader pressure trace.
- Dependency-free trace generation and trace-runner check validation.
- Documentation and compatibility matrix updates.

Out of scope:

- Running Docker-backed MariaDB or RQG in default CI.
- Changing ownerless pressure behavior.
- SQL-level table-lock fault injection.
- Broader randomized DDL/file lifecycle stress.

## Compatibility Impact

No MyLite SQL, C API, runtime, or storage behavior changes. The generated trace
remains MariaDB-compatible SQL for external harnesses.

## Directory And Lifecycle Impact

No MyLite database-directory layout changes. Generated files remain under the
caller-provided output directory.

## Native Storage Impact

No MyLite native storage changes. The trace adds ordinary MariaDB InnoDB
replacement-copy DDL over external harness tables.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice modifies a shell trace generator
and docs only.

## Test Plan

- Run `tools/ownerless-active-reader-pressure-trace --rounds 4 --rows 3
  --reader-polls 6 --output <tmp> --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir <tmp> --check`.
- Run the focused tools CTest subset for the active-reader pressure trace,
  trace runner, and pressure scaled trace suite.
- Run `git diff --check`, `format-check`, and cached diff checks.

## Acceptance Criteria

- `schema.sql` contains both replacement-copy SQL families.
- `expected.sql` checks replacement-copy row counts, value sums, copied index
  or copied column metadata, and old metadata absence.
- `manifest.txt` records replacement-copy expected sums.
- The trace runner accepts the generated package in check mode.

## Risks And Follow-Up

- The trace extends deterministic external harness input; it is not a
  substitute for long-running randomized MariaDB/RQG stress.
- Replacement-copy SQL is emitted by the worker after deterministic update
  rounds. External harnesses can still replay it concurrently with the reader,
  but the existing runner does not enforce a strict reader-before-DDL barrier.
