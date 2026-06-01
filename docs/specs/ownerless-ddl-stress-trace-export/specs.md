# Ownerless DDL Stress Trace Export

## Problem

The ownerless stress preset already runs deterministic concurrent DDL/DML
stress inside MyLite, but the Phase 12 validation gap still calls out external
MariaDB/RQG-style randomized DDL oracle execution. A full external runner is
larger than one correctness slice. The existing deterministic DDL/DML schedule
can still be exported as SQL traces, giving external runners a stable workload
and final oracle while keeping the full external execution requirement planned.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:5228` defines `mysql_create_table()` for the
  ordinary SQL `CREATE TABLE` path.
- `mariadb/sql/sql_table.cc:10703` defines `mysql_alter_table()` for the
  `ALTER TABLE ... ADD COLUMN` and online index DDL path.
- `mariadb/sql/sql_rename.cc:56` defines `mysql_rename_tables()` for the
  `RENAME TABLE` path.
- `mariadb/sql/sql_table.cc:1339` defines `mysql_rm_table_no_locks()` for
  table drop cleanup after metadata lock acquisition.
- `mariadb/storage/innobase/handler/ha_innodb.cc:13926` defines
  `ha_innobase::truncate()` for InnoDB table truncate handling.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_concurrent_ddl_stress()` starts three DDL workers, two DML
  workers, and one aggregate reader, then checks ownerless/native reopen before
  and after forced `.shm` rebuild.
- The same file's `run_ownerless_ddl_stress_worker()`,
  `run_ownerless_ddl_stress_dml_worker()`,
  `run_ownerless_ddl_stress_reader()`, and
  `assert_ownerless_ddl_stress_state()` define the deterministic schedule and
  final aggregate/metadata oracle.

## Scope And Non-Goals

In scope:

- Add a dependency-free repository tool that writes SQL traces matching the
  deterministic ownerless DDL/DML stress schedule.
- Emit schema SQL, DDL worker SQL, DML worker SQL, reader SQL, expected final
  oracle SQL, and a manifest.
- Add a CTest smoke check that generates a small trace and validates expected
  files and oracle fields.
- Update compatibility and ownerless-concurrency docs to mark DDL trace export
  as covered while full external oracle execution remains planned.

Out of scope:

- Starting or managing an external MariaDB server.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or the in-process DDL stress schedule.
- Claiming SQL-level table-lock wait fault injection coverage.

## Design

`tools/ownerless-ddl-stress-trace` accepts `--output DIR`, `--rounds N`, and
`--check`. It writes:

- `schema.sql`, which creates `app.ownerless_sql` with the same initial rows
  used by the DDL stress oracle and drops deterministic leftover stress tables
  from a previous interrupted replay,
- `ddl-worker-1.sql` through `ddl-worker-3.sql`, each creating a per-round
  InnoDB table, inserting rows, adding a defaulted column, adding an online
  secondary index, renaming, verifying the defaulted column, truncating,
  verifying emptiness, and dropping the renamed table,
- `dml-worker-1.sql` and `dml-worker-2.sql`, each repeatedly incrementing one
  stable row and emitting deterministic per-row checks,
- `reader.sql`, which polls the aggregate sum and checks monotonic bounds while
  DDL and DML workers run,
- `expected.sql`, which reports the final aggregate and remaining stress-table
  count and returns `ok` only when they match the deterministic oracle,
- `manifest.txt`, which records constants and expected totals.

External harnesses can run `schema.sql`, execute the worker and reader files
concurrently, and then run `expected.sql` against MariaDB or another compatible
target. The smoke CTest uses `--check` to prove trace generation without
requiring an external server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The generated traces make the existing DDL/DML stress schedule
portable to external MariaDB/RQG-style validation.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL uses ordinary
MariaDB DDL and DML so an external runner can own its own schema and data
directory.

## Native Storage Impact

No native storage runtime changes. When replayed externally, the trace covers
ordinary InnoDB create, alter, online index, rename, truncate, and drop paths
plus concurrent DML over a stable table.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds a shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-ddl-stress-trace --rounds 3 --output <tmp> --check`.
- Run `ctest --preset embedded-dev -R tools.ownerless-ddl-stress-trace
  --output-on-failure`.
- Run focused ownerless DDL stress to prove the source schedule still passes.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, DDL worker, DML worker, reader,
  expected, and manifest files.
- The smoke test verifies representative DDL, DML, reader, and final oracle
  fields are emitted.
- Documentation and compatibility notes mark DDL trace export as covered while
  full external MariaDB/RQG execution remains planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas;
  future changes to those formulas must update the tool in the same slice.
- The generated files are external harness inputs, not a replacement for
  actually running long-lived MariaDB/RQG workloads.
