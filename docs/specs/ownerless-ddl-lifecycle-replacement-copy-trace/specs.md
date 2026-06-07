# Ownerless DDL Lifecycle Replacement Copy Trace

## Problem

The deterministic DDL lifecycle trace already exports create, rename,
truncate, force-rebuild, drop, same-name recreate, and InnoDB `SPACE` identity
oracles for external MariaDB/RQG-style harnesses. Ownerless replacement-copy
coverage has since expanded for `CREATE OR REPLACE TABLE ... LIKE` and
`CREATE OR REPLACE TABLE ... AS SELECT`; the DDL lifecycle trace should carry
those same destructive replacement-copy spellings so external harness input
covers the broader file-lifecycle class.

This slice extends trace generation and docs only.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc` implements `CREATE TABLE ... LIKE`,
  `CREATE TABLE ... SELECT`, and `CREATE OR REPLACE TABLE` replacement paths.
- `tools/ownerless-ddl-lifecycle-trace` emits a deterministic trace package
  for external runners and already records final table metadata, aggregate, and
  InnoDB `SPACE` identity oracles.
- `tools/ownerless-sql-trace-runner --check` validates the generated package
  without requiring an external MariaDB server.

## Design

Extend `tools/ownerless-ddl-lifecycle-trace`:

1. Seed `LIKE` and CTAS source tables plus old replacement targets in
   `schema.sql`.
2. During each worker round, execute `CREATE OR REPLACE TABLE ... LIKE`, insert
   copied rows through the copied table shape, and execute
   `CREATE OR REPLACE TABLE ... AS SELECT`.
3. Add per-round worker checks for replacement-copy aggregate values.
4. Add final expected oracles for copied rows, copied secondary-index metadata,
   copied CTAS column metadata, and old replacement metadata absence.
5. Record replacement-copy expected sums in `manifest.txt`.
6. Extend `--check` validation for the new SQL and oracle names.

## Scope And Non-Goals

In scope:

- Deterministic external trace input for replacement-copy DDL inside the DDL
  lifecycle trace.
- Dependency-free trace generation and trace-runner validation.
- Documentation and compatibility updates.

Out of scope:

- Running Docker-backed MariaDB or RQG in default CI.
- Changing ownerless runtime DDL/recovery behavior.
- SQL-level table-lock fault injection.
- Claiming new Docker-backed replay evidence for this trace extension.

## Compatibility Impact

No MyLite SQL, public C API, runtime, or storage behavior changes. The generated
trace remains MariaDB-compatible SQL for external harnesses.

## Directory And Lifecycle Impact

No MyLite database-directory layout changes. The generated trace exercises
external MariaDB-compatible InnoDB DDL when replayed by a harness.

## Native Storage Impact

No native storage format changes. The added SQL uses ordinary InnoDB
replacement-copy DDL and final metadata oracles.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change modifies one shell trace exporter
and docs.

## Test Plan

- Run `tools/ownerless-ddl-lifecycle-trace --rounds 3 --output <tmp> --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir <tmp> --check`.
- Run focused CTest for `tools.ownerless-ddl-lifecycle-trace` and
  `tools.ownerless-sql-trace-suite`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- `worker-1.sql` contains both replacement-copy DDL forms.
- `expected.sql` verifies replacement-copy rows, value sums, copied metadata,
  and old metadata absence.
- `manifest.txt` records replacement-copy expected sums.
- The trace runner accepts the generated trace package in check mode.

## Risks And Follow-Up

- This extends deterministic check-mode and external-harness input, but it does
  not replace Docker-backed replay or long-running randomized RQG for the new
  trace shape.
