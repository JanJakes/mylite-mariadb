# Ownerless Temporary Table Stress Trace Export

## Problem

The ownerless temporary-table stress test proves that concurrent processes can
churn same-named InnoDB temporary tables while a permanent table with the same
name can be created after those worker sessions finish. Phase 12 still calls
for external MariaDB/RQG-style stress with oracles. A full external runner is
larger than one correctness slice, but the deterministic temporary-table
schedule can be exported as SQL traces and a durable final oracle.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:4673-4729` handles temporary-table create
  branches in `mysql_create_table_no_lock()`.
- `mariadb/sql/sql_table.cc:1488-1519` handles the temporary-table drop path
  inside `mysql_rm_table_no_locks()`.
- `mariadb/sql/sql_table.cc:1203` notes that temporary-table locks are local to
  the session.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_temporary_table_stress()` starts four ownerless worker
  processes, then creates a permanent `app.ownerless_temp_stress` table after
  workers finish and verifies ownerless/native reopen before and after forced
  `.shm` rebuild.
- The same file's `run_ownerless_temp_stress_worker()` and
  `assert_ownerless_temp_stress_permanent_table()` define the deterministic
  same-name temporary-table schedule and durable final oracle.

## Scope And Non-Goals

In scope:

- Add a dependency-free repository tool that writes SQL traces matching the
  deterministic ownerless temporary-table stress schedule.
- Emit schema SQL, one worker SQL file per temporary-table worker, post-worker
  SQL that creates the permanent table, expected final oracle SQL, and a
  manifest.
- Add a CTest smoke check that generates a small trace and validates expected
  files and oracle fields.
- Update compatibility and ownerless-concurrency docs to mark temporary-table
  trace export as covered while full external oracle execution remains planned.

Out of scope:

- Starting or managing an external MariaDB server.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or the in-process temporary-table stress
  schedule.
- Claiming crash-in-action coverage for external temporary-table sessions.

## Design

`tools/ownerless-temporary-table-stress-trace` accepts `--output DIR`,
`--rounds N`, and `--check`. It writes:

- `schema.sql`, which creates `app.ownerless_sql` with the stable rows used by
  the worker sanity check and removes any previous permanent
  `ownerless_temp_stress` table,
- `worker-1.sql` through `worker-4.sql`, each intended for a separate session
  and repeatedly creating, reading, updating, and dropping a same-named
  `TEMPORARY TABLE ownerless_temp_stress`,
- `post.sql`, which must be run after worker sessions finish and creates the
  permanent `ownerless_temp_stress` table with the durable final row,
- `expected.sql`, which reports the permanent table sum and returns `ok` only
  when it matches the deterministic oracle,
- `manifest.txt`, which records constants and expected totals.

External harnesses can run `schema.sql`, execute worker files concurrently in
separate sessions, run `post.sql`, and then run `expected.sql`. The smoke CTest
uses `--check` to prove trace generation without requiring an external server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The generated trace makes the existing deterministic temporary-table
stress schedule portable to external MariaDB/RQG-style validation.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL uses ordinary
MariaDB DDL and temporary-table semantics so an external runner can own its own
schema and data directory.

## Native Storage Impact

No native storage runtime changes. When replayed externally, the trace covers
same-name InnoDB temporary-table creation/drop in concurrent sessions plus a
permanent InnoDB table with the same name after temporary sessions finish.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds a shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-temporary-table-stress-trace --rounds 3 --output <tmp>
  --check`.
- Run `ctest --preset embedded-dev -R
  tools.ownerless-temporary-table-stress-trace --output-on-failure`.
- Run focused ownerless temporary-table stress to prove the source schedule
  still passes.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, worker, post, expected, and manifest
  files.
- The smoke test verifies temporary-table churn checks and the final permanent
  table oracle are emitted.
- Documentation and compatibility notes mark temporary-table trace export as
  covered while full external MariaDB/RQG execution remains planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas;
  future changes to those formulas must update the tool in the same slice.
- The generated files are external harness inputs, not a replacement for
  actually running long-lived MariaDB/RQG workloads.
