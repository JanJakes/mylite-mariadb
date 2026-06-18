# Ownerless CTAS DML Trace Export

## Problem Statement

Ownerless SQL coverage now exercises DML immediately after
`CREATE TABLE ... SELECT`, including the page-version refresh case that exposed
newer native pages being overwritten by older retained snapshot-boundary pages.
That coverage is embedded-only. The remaining external MariaDB/RQG gap needs
deterministic SQL input that can be replayed by the existing trace-runner and
optional MariaDB Docker smoke tools without adding long-running randomized
stress to default CI.

This slice adds a bounded deterministic CTAS post-create DML trace family.

## Source Findings

- MariaDB 11.8 remains the compatibility authority for CTAS and subsequent DML
  behavior.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  focused `ctas-post-create-dml` ownerless coverage for CTAS-created InnoDB
  tables, post-create numeric/payload updates, retained page-version WAL while
  a stale reader is live, and ownerless/native reopen checks.
- The `active-reader-pressure-write-policy` selector already covers
  write-throttle behavior for `UPDATE` and `DELETE` against an existing CTAS
  destination.
- `tools/ownerless-sql-trace-suite` is the deterministic external-harness
  trace registry, and `tools/ownerless-sql-trace-runner` accepts traces with
  `schema.sql`, one or more concurrent SQL files, `expected.sql`, and
  `manifest.txt`.

## Design

Add `tools/ownerless-ctas-dml-trace`, which generates:

- `schema.sql` with an `app.ownerless_sql` aggregate table and an InnoDB CTAS
  source table containing three 4000-byte payload rows.
- `worker-1.sql` that repeats a bounded sequence: drop the destination, create
  `ownerless_ctas_dml` with `CREATE TABLE ... ENGINE=InnoDB AS SELECT`, update
  two rows, delete one row, insert one replacement row, mutate the stable
  aggregate table, and emit a per-round oracle.
- `schema.sql` also owns a retry-aware reader procedure, and `reader.sql` calls
  it. The procedure polls repeatable-read consistent snapshots, checks the
  aggregate table stays monotonic and below the final total, and retries each
  poll on ordinary external MariaDB `1020`, `1205`, `1213`, or SQLSTATE
  `40001` contention before the final oracle runs.
- `expected.sql` with final row-count, id-sum, value-sum, payload-byte, table,
  and column oracles.
- `manifest.txt` with the derived counts and reader retry limit used by
  external harnesses.

Register the trace in `tools/ownerless-sql-trace-suite` as `ctas-dml` and add a
dependency-free CTest smoke check for the exporter. Follow-up evidence first
runs the new trace through the optional MariaDB Docker smoke tool at scale 2;
later full-suite evidence records the current 12 trace families together.
Neither path claims randomized RQG coverage.

## Scope

In scope:

- Deterministic CTAS post-create `UPDATE`, `DELETE`, and `INSERT` SQL trace
  generation.
- Repeatable-read reader polling to keep the trace aligned with ownerless
  snapshot-pressure shapes.
- Trace-runner compatibility and suite registration.
- CTest check-mode validation for the exporter.
- Compatibility and ownerless concurrency documentation updates.

Out of scope:

- Product runtime changes.
- Randomized RQG or SQLancer generation.
- Adding Docker replay to default CI.
- Exhaustive CTAS crash or DML-shape matrices.

## Compatibility Impact

No MyLite SQL behavior changes. The slice adds reusable external-harness input
for MariaDB-compatible CTAS post-create DML behavior that is already covered in
focused embedded ownerless tests.

The deterministic trace suite now has 12 trace families in check mode after
compressed row-format DDL export. Existing full scale-2 Docker-backed MariaDB
evidence remains historical evidence for the 10- and 11-family suites present
in those replays. Focused `ctas-dml` Docker-backed MariaDB 11.8 replay has
separate scale-2 evidence, and the current 12-family scale-2 replay covered
the whole deterministic suite after CTAS reader retry hardening.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. The generated trace files live
under the caller-provided output directory, and real replay tools mutate only
their target SQL database.

## Native Storage Impact

No native-storage format changes. The trace uses InnoDB tables so it remains
compatible with the ownerless InnoDB-only external evidence path.

## Public API, Build, Size, And Dependencies

No public API, production binary-size, license, or runtime dependency changes.
The slice adds one shell tool and one CTest registration.

## Test Plan

- Run `bash -n tools/ownerless-ctas-dml-trace`.
- Run `tools/ownerless-ctas-dml-trace --output DIR --rounds 3 --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir DIR --check`.
- Run `tools/ownerless-sql-trace-suite --output DIR --trace ctas-dml --check`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --trace
  ctas-dml --scale 2`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 2`
  after the current suite grows beyond the previous full replay.
- Run the focused CTest for `tools.ownerless-ctas-dml-trace`.
- Run the full dependency-free deterministic trace-suite CTest after CMake
  reconfiguration.
- Run `bash -n` over the affected shell tools, `format-check`,
  `git diff --check`, cached diff checks, and cleanup checks.

## Acceptance Criteria

- The CTAS DML exporter emits the trace-runner contract files.
- Check mode proves the generated worker includes CTAS create, update, delete,
  and insert statements, the reader calls the retry-aware consistent-snapshot
  procedure, and the final oracle exists.
- External replay survives ordinary raw-client reader contention through a
  bounded retry contract.
- The trace suite includes `ctas-dml` and can generate it by focused
  `--trace`.
- Documentation states that CTAS DML is deterministic trace-export evidence,
  while external MariaDB/RQG stress remains planned.

## Evidence

Initial local validation passed:

```text
tools/ownerless-ctas-dml-trace --output /tmp/mylite-ownerless-ctas-dml-trace-check --rounds 3 --check
tools/ownerless-sql-trace-runner --trace-dir /tmp/mylite-ownerless-ctas-dml-trace-check --check
tools/ownerless-sql-trace-suite --output /tmp/mylite-ownerless-suite-ctas-dml-check --trace ctas-dml --check
```

The focused suite check reported `trace_count=1` for `ctas-dml`.

Focused Docker-backed MariaDB 11.8 replay at scale 2 then passed:

```text
trace=ctas-dml
scale=2
trace_count=1
suite_run=ok
external_mariadb_trace_smoke=ok
```

The generated CTAS DML manifest reported:

```text
rounds=8
reader_polls=64
expected_rows=3
expected_id_sum=247
expected_value_sum=3021
expected_payload_bytes=12000
```

The final oracle reported:

```text
observed_rows observed_id_sum observed_value_sum observed_payload_bytes
3 247 3021 12000
ownerless_ctas_dml_trace_check
ok
```

The first full 12-family Docker-backed MariaDB 11.8 replay attempt later
exposed the missing reader retry contract:

```text
ERROR 1020 (HY000) at line 274: Record has changed since last read in table
'ownerless_sql'; try restarting transaction
```

After moving the CTAS reader into a bounded-retry procedure, focused scale-2
replay passed again with `retry_limit=40` and
`ownerless_ctas_dml_trace_reader_retries=0` in that run. The current full
12-family scale-2 replay also passed:

```text
scale=2
trace_count=12
trace=independent-table-stress
trace=random-tx
trace=fk-graph
trace=ddl-stress
trace=ddl-lifecycle
trace=ctas-dml
trace=checksum-stress
trace=transaction-stress
trace=temporary-table-stress
trace=active-reader-pressure
trace=compressed-row-format-ddl
trace=blob-pressure
suite_run=ok
external_mariadb_trace_smoke=ok
```

## Risks And Unresolved Questions

- The trace is deterministic and bounded; it does not replace randomized
  external MariaDB/RQG stress.
- Full 12-family Docker-backed replay was recorded later, but it remains
  deterministic rather than randomized.
- Exhaustive CTAS post-create DML crash and isolation matrices remain planned.
