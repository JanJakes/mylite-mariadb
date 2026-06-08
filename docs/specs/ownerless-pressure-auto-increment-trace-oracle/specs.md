# Ownerless Pressure AUTO_INCREMENT Trace Oracle

## Problem Statement

The ownerless active-reader pressure write-policy selector now proves
`ALTER TABLE ... AUTO_INCREMENT = N` is throttled while retained page-version
WAL is at the active-reader limit, and that the blocked ALTER does not advance
the native high watermark. The deterministic active-reader pressure trace
exporter still only carried large-row DML and replacement-copy DDL oracles, so
external MariaDB/RQG-style replay input did not include the AUTO_INCREMENT
high-watermark DDL shape.

This slice extends that existing trace package with a small deterministic
AUTO_INCREMENT table and final oracle. It does not change MyLite runtime
behavior or claim that SQL-level table-lock fault injection is reachable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE` as data-changing,
  write-logged, autocommit, insert-data, and admin work through
  `sql_command_flags[SQLCOM_ALTER_TABLE]`.
- `mariadb/sql/sql_yacc.yy` stores the parsed `AUTO_INCREMENT = N` value in
  `Lex->create_info.auto_increment_value`.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` copies the current
  handler AUTO_INCREMENT value into `create_info->auto_increment_value` when
  no explicit `HA_CREATE_USED_AUTO` option is supplied.
- `mariadb/storage/innobase/handler/handler0alter.cc:commit_set_autoinc()`
  persists an explicit user-supplied AUTO_INCREMENT value through
  `btr_write_autoinc()` and mimics copy-alter semantics when a requested value
  is below the current maximum row value.
- `tools/ownerless-active-reader-pressure-trace` already exports the
  active-reader pressure family for external runners as `schema.sql`,
  `worker-1.sql`, `reader.sql`, `expected.sql`, and `manifest.txt`.

## Design

Extend `tools/ownerless-active-reader-pressure-trace` so `schema.sql` also
creates `app.ownerless_active_reader_auto_inc`:

```sql
CREATE TABLE ownerless_active_reader_auto_inc (
  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  value INT NOT NULL
) ENGINE=InnoDB;
INSERT INTO ownerless_active_reader_auto_inc (value) VALUES (10), (20);
```

The existing worker procedure remains the only writer. After its deterministic
large-row updates and replacement-copy DDL, it performs:

```sql
INSERT INTO ownerless_active_reader_auto_inc (value) VALUES (30);
ALTER TABLE ownerless_active_reader_auto_inc AUTO_INCREMENT = 100;
INSERT INTO ownerless_active_reader_auto_inc (value) VALUES (1000);
```

The generated worker and `expected.sql` both verify:

- row count `4`;
- `SUM(id) = 106`;
- `MAX(id) = 100`;
- `SUM(value) = 1060`.

The manifest records the same constants so external harnesses can report the
oracle values without parsing SQL.

## Scope

In scope:

- Deterministic external trace input for AUTO_INCREMENT high-watermark DDL in
  the active-reader pressure family.
- Dependency-free generator `--check` coverage and trace-suite check coverage.
- Documentation and compatibility-matrix updates.

Out of scope:

- MyLite ownerless runtime changes.
- SQL-level table-lock fault injection.
- Docker-backed external MariaDB replay in CI.
- Long-running MariaDB/RQG randomized pressure execution.

## Compatibility Impact

No SQL, C API, PHP API, storage format, or directory-lifecycle behavior
changes. The generated SQL uses ordinary MariaDB-compatible InnoDB
AUTO_INCREMENT semantics as an external oracle for the pressure family.

## Directory And Lifecycle Impact

No MyLite database-directory changes. The generated files remain under the
caller-provided output directory and target an external MariaDB-compatible
server only when replayed by a harness.

## Native Storage Impact

No native storage format changes. The oracle uses an InnoDB table with a
single AUTO_INCREMENT primary key and a single explicit high-watermark ALTER.

## Public API Impact

No public API changes.

## Build And Binary-Size Impact

No production binary-size impact. The slice changes one shell trace exporter
and documentation.

## Test Plan

- Run `bash -n tools/ownerless-active-reader-pressure-trace`.
- Run `tools/ownerless-active-reader-pressure-trace --rounds 4 --rows 3
  --reader-polls 6 --output <tmp> --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir <tmp> --check`.
- Run `ctest --preset prod -R
  'tools.ownerless-(active-reader-pressure-trace|sql-trace-suite-pressure-scaled|sql-trace-runner)$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- `schema.sql` creates and seeds the AUTO_INCREMENT oracle table.
- The worker performs one implicit insert before
  `ALTER TABLE ... AUTO_INCREMENT = 100` and one implicit insert after it.
- Worker and final expected SQL both verify count, ID sum, maximum ID, and
  value sum.
- `manifest.txt` records the AUTO_INCREMENT expected constants.
- Existing active-reader snapshot, replacement-copy DDL, retry, and final
  aggregate oracles remain intact.

## Verification Results

Local verification on 2026-06-08 used the production `prod` CTest preset and a
focused disposable MariaDB 11.8 Docker replay for the generated active-reader
pressure trace.

- `bash -n tools/ownerless-active-reader-pressure-trace`: passed.
- `tools/ownerless-active-reader-pressure-trace --rounds 4 --rows 3
  --reader-polls 6 --output build/manual-active-reader-pressure-trace-smoke
  --check`: passed and emitted `auto_inc_expected_id_sum=106` and
  `auto_inc_expected_max_id=100`.
- `tools/ownerless-sql-trace-runner --trace-dir
  build/manual-active-reader-pressure-trace-smoke --check`: passed with
  `concurrent_count=2`.
- `ctest --preset prod -R
  'tools\\.ownerless-(active-reader-pressure-trace|sql-trace-suite-pressure-scaled|sql-trace-runner)$'
  --output-on-failure`: passed, 3/3 tests, 0.92s.
- `tools/ownerless-external-mariadb-trace-smoke --output
  build/manual-external-active-reader-auto-inc-smoke --trace
  active-reader-pressure --scale 2`: passed with `trace_count=1`,
  `suite_run=ok`, and `external_mariadb_trace_smoke=ok`.

## Risks And Unresolved Questions

- This is deterministic external-harness input, not product pressure throttling
  behavior. The product throttling path remains covered by the embedded
  ownerless SQL selector.
- External MariaDB replay may still observe ordinary snapshot or lock
  contention, handled by the existing retry procedures. Final-state mismatches
  are not hidden because `expected.sql` checks the exact oracle.
- Full external MariaDB/RQG long-running pressure stress remains planned.
