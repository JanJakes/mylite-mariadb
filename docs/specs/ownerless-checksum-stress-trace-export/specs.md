# Ownerless Checksum Stress Trace Export

## Problem

The ownerless checksum stress test already uses a deterministic final oracle
for concurrent writers over one shared InnoDB table. Phase 12 still calls for
external MariaDB/RQG-style long-running stress with checksum oracles. A full
external runner remains larger than one correctness slice, but the existing
checksum schedule can be exported as SQL traces and an aggregate oracle so an
external harness can replay the same workload against MariaDB-compatible
targets.

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
  `row_update_for_mysql()`, and `mariadb/storage/innobase/row/row0upd.cc:2810`
  defines `row_upd_step()` for InnoDB update execution.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_checksum_stress()` creates `app.ownerless_checksum_stress`,
  starts four writer workers and one aggregate reader, and verifies
  ownerless/native reopen before and after forced `.shm` rebuild.
- The same file's `ownerless_checksum_stress_row_id()`,
  `ownerless_checksum_stress_delta()`,
  `ownerless_checksum_stress_expected()`,
  `run_ownerless_checksum_stress_writer()`,
  `run_ownerless_prepared_checksum_stress_writer()`, and
  `assert_ownerless_checksum_stress_totals()` define the deterministic schedule
  and final count, sum, version, and weighted-sum oracle.

## Scope And Non-Goals

In scope:

- Add a dependency-free repository tool that writes SQL traces matching the
  deterministic ownerless checksum stress schedule.
- Emit schema SQL, one worker SQL file per checksum worker, reader SQL,
  expected final oracle SQL, and a manifest.
- Preserve the C test's direct/prepared worker distinction as trace metadata
  comments while emitting ordinary SQL that an external harness can run.
- Add a CTest smoke check that generates a small trace and validates expected
  files and oracle fields.
- Update compatibility and ownerless-concurrency docs to mark checksum trace
  export as covered while full external oracle execution remains planned.

Out of scope:

- Starting or managing an external MariaDB server.
- Running RQG, SQLancer, or long-running external stress in CI.
- Changing ownerless runtime behavior or the in-process checksum stress
  schedule.
- Reproducing prepared-statement APIs inside plain SQL files.

## Design

`tools/ownerless-checksum-stress-trace` accepts `--output DIR`, `--rounds N`,
and `--check`. It writes:

- `schema.sql`, which creates `app.ownerless_checksum_stress` and inserts the
  same worker-owned row partitions as the C stress test,
- `worker-1.sql` through `worker-4.sql`, each containing that worker's
  deterministic update schedule and periodic version-progress checks,
- `reader.sql`, which polls aggregate sum/version totals and checks monotonic
  bounds while workers run,
- `expected.sql`, which reports observed count, sum, version count, and
  weighted sum and returns `ok` only when they match the deterministic oracle,
- `manifest.txt`, which records constants and expected totals.

External harnesses can run `schema.sql`, execute worker and reader files
concurrently, then run `expected.sql`. The smoke CTest uses `--check` to prove
trace generation without requiring an external server.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The generated trace makes the existing deterministic checksum stress
schedule portable to external MariaDB/RQG-style validation.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL uses ordinary
MariaDB DDL and DML so an external runner can own its own schema and data
directory.

## Native Storage Impact

No native storage runtime changes. When replayed externally, the trace covers
ordinary InnoDB row updates and aggregate reads over one shared table.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds a shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-checksum-stress-trace --rounds 3 --output <tmp>
  --check`.
- Run `ctest --preset embedded-dev -R tools.ownerless-checksum-stress-trace
  --output-on-failure`.
- Run focused ownerless checksum stress to prove the source schedule still
  passes.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The tool generates non-empty schema, worker, reader, expected, and manifest
  files.
- The smoke test verifies deterministic aggregate oracle values are emitted.
- Documentation and compatibility notes mark checksum trace export as covered
  while full external MariaDB/RQG execution remains planned.

## Risks And Follow-Up

- The trace exporter mirrors the current deterministic C stress formulas;
  future changes to those formulas must update the tool in the same slice.
- The generated files are external harness inputs, not a replacement for
  actually running long-lived MariaDB/RQG workloads.
