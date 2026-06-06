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
- `reader.sql` that polls repeatable-read consistent snapshots and checks the
  aggregate table stays monotonic and below the final total.
- `expected.sql` with final row-count, id-sum, value-sum, payload-byte, table,
  and column oracles.
- `manifest.txt` with the derived counts used by external harnesses.

Register the trace in `tools/ownerless-sql-trace-suite` as `ctas-dml` and add a
dependency-free CTest smoke check for the exporter. Follow-up evidence runs the
new trace through the optional MariaDB Docker smoke tool at scale 2 without
claiming full 11-family replay or randomized RQG coverage.

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
- Full 11-family Docker-backed replay.
- Exhaustive CTAS crash or DML-shape matrices.

## Compatibility Impact

No MyLite SQL behavior changes. The slice adds reusable external-harness input
for MariaDB-compatible CTAS post-create DML behavior that is already covered in
focused embedded ownerless tests.

The deterministic trace suite now has 11 trace families in check mode. Existing
full scale-2 Docker-backed MariaDB evidence remains historical evidence for the
10 trace families present in that replay. Focused `ctas-dml` Docker-backed
MariaDB 11.8 replay now has separate scale-2 evidence.

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

- Run `tools/ownerless-ctas-dml-trace --output DIR --rounds 3 --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir DIR --check`.
- Run `tools/ownerless-sql-trace-suite --output DIR --trace ctas-dml --check`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --trace
  ctas-dml --scale 2`.
- Run the focused CTest for `tools.ownerless-ctas-dml-trace`.
- Run the full dependency-free deterministic trace-suite CTest after CMake
  reconfiguration.
- Run `bash -n` over the affected shell tools, `format-check`,
  `git diff --check`, cached diff checks, and cleanup checks.

## Acceptance Criteria

- The CTAS DML exporter emits the trace-runner contract files.
- Check mode proves the generated worker includes CTAS create, update, delete,
  and insert statements, the reader uses consistent snapshots, and the final
  oracle exists.
- The trace suite includes `ctas-dml` and can generate it by focused
  `--trace`.
- Documentation states that CTAS DML is deterministic trace-export evidence,
  while full 11-family external replay and external MariaDB/RQG stress remain
  planned.

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

## Risks And Unresolved Questions

- The trace is deterministic and bounded; it does not replace randomized
  external MariaDB/RQG stress.
- Full 11-family Docker-backed replay remains planned because the earlier
  full-suite scale-2 evidence covered the 10-family suite that existed before
  CTAS DML export.
- Exhaustive CTAS post-create DML crash and isolation matrices remain planned.
