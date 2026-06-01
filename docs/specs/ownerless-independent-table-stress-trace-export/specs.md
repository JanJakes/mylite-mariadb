# Ownerless Independent Table Stress Trace Export

## Problem

The ownerless independent-table stress test proves that four read/write
processes can update separate InnoDB tables while a reader repeatedly observes
the combined total. Phase 12 still calls for external MariaDB/RQG-style stress
with oracles. A full external runner remains larger than one correctness
slice, but the deterministic independent-table schedule can be exported as SQL
traces and a final aggregate oracle.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc:346` defines
  `Sql_cmd_update::update_single_table()` for ordinary single-table SQL
  `UPDATE` execution.
- `mariadb/storage/innobase/handler/ha_innodb.cc:8598` defines
  `ha_innobase::update_row()`, which reaches
  `row_update_for_mysql()` for InnoDB row updates.
- `mariadb/storage/innobase/row/row0mysql.cc:1588` defines
  `row_update_for_mysql()` for update/delete execution from the MySQL handler.
- `mariadb/storage/innobase/handler/ha_innodb.cc:16182` defines
  `ha_innobase::external_lock()`, where autocommit statement-end handling and
  InnoDB consistent-read behavior are coordinated.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_independent_table_stress()` creates four
  `app.ownerless_stress_N` InnoDB tables, starts four writer processes and one
  aggregate reader, and verifies ownerless/native reopen before and after
  forced `.shm` rebuild.
- The same file's `run_ownerless_stress_writer()`,
  `run_ownerless_stress_reader()`, `ownerless_stress_iterations()`,
  `ownerless_stress_reader_polls()`, and
  `assert_ownerless_stress_total()` define the deterministic schedule and
  final total oracle.

## Scope And Non-Goals

In scope:

- Add a dependency-free repository tool that writes SQL traces matching the
  deterministic ownerless independent-table stress schedule.
- Emit schema SQL, one worker SQL file per independent table, aggregate reader
  SQL, expected final oracle SQL, and a manifest.
- Add a CTest smoke check that generates a small trace and validates expected
  files and oracle fields.
- Update compatibility and ownerless-concurrency docs to mark independent-table
  trace export as covered while full external oracle execution remains planned.

Out of scope:

- Starting or managing an external MariaDB server.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or the in-process independent-table
  stress schedule.
- Claiming coverage for randomized cross-table write conflicts; this stress
  shape intentionally keeps writers on separate tables and uses the reader as
  the cross-process visibility oracle.

## Design

`tools/ownerless-independent-table-stress-trace` accepts `--output DIR`,
`--iterations N`, `--reader-polls N`, and `--check`. It writes:

- `schema.sql`, which creates `app.ownerless_stress_1` through
  `app.ownerless_stress_4` with one row initialized to zero in each table,
- `worker-1.sql` through `worker-4.sql`, each intended for a separate session
  and repeatedly incrementing its table's row with periodic exact-value checks,
- `reader.sql`, which repeatedly sums all four tables and checks that the
  observed total is monotonic and bounded by the deterministic final total,
- `expected.sql`, which reports row count, sum, min, and max over the four
  table rows and returns `ok` only when every table reached the configured
  iteration count,
- `manifest.txt`, which records constants and expected totals.

External harnesses can run `schema.sql`, execute worker and reader files
concurrently, then run `expected.sql`. The smoke CTest uses `--check` to prove
trace generation without requiring an external server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The generated trace makes the existing deterministic independent-table
stress schedule portable to external MariaDB/RQG-style validation.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL uses ordinary
MariaDB DDL, DML, and autocommit reads so an external runner can own its own
schema and data directory.

## Native Storage Impact

No native storage runtime changes. When replayed externally, the trace covers
concurrent InnoDB autocommit updates on independent tables plus aggregate reads
that observe committed progress.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds a shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-independent-table-stress-trace --iterations 3
  --reader-polls 6 --output <tmp> --check`.
- Run `ctest --preset embedded-dev -R
  tools.ownerless-independent-table-stress-trace --output-on-failure`.
- Run focused ownerless independent-table stress to prove the source schedule
  still passes.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, worker, reader, expected, and manifest
  files.
- The smoke test verifies independent writer checks, reader bounds checks, and
  final aggregate oracle values are emitted.
- Documentation and compatibility notes mark independent-table trace export as
  covered while full external MariaDB/RQG execution remains planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas;
  future changes to those formulas must update the tool in the same slice.
- The generated files are external harness inputs, not a replacement for
  actually running long-lived MariaDB/RQG workloads.
