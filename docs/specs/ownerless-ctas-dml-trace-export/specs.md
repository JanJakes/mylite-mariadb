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
dependency-free CTest smoke check for the exporter. The trace intentionally
does not claim external Docker replay evidence until the optional MariaDB smoke
tool is run for this new family.

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
- Claiming Docker-backed replay for the new CTAS DML trace before it has been
  run.
- Exhaustive CTAS crash or DML-shape matrices.

## Compatibility Impact

No MyLite SQL behavior changes. The slice adds reusable external-harness input
for MariaDB-compatible CTAS post-create DML behavior that is already covered in
focused embedded ownerless tests.

The deterministic trace suite now has 11 trace families in check mode. Existing
full scale-2 Docker-backed MariaDB evidence remains historical evidence for the
10 trace families present in that replay.

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
  while full external MariaDB/RQG stress remains planned.

## Evidence

Initial local validation passed:

```text
tools/ownerless-ctas-dml-trace --output /tmp/mylite-ownerless-ctas-dml-trace-check --rounds 3 --check
tools/ownerless-sql-trace-runner --trace-dir /tmp/mylite-ownerless-ctas-dml-trace-check --check
tools/ownerless-sql-trace-suite --output /tmp/mylite-ownerless-suite-ctas-dml-check --trace ctas-dml --check
```

The focused suite check reported `trace_count=1` for `ctas-dml`.

## Risks And Unresolved Questions

- External Docker-backed MariaDB replay for `ctas-dml` is still pending.
- The trace is deterministic and bounded; it does not replace randomized
  external MariaDB/RQG stress.
- Exhaustive CTAS post-create DML crash and isolation matrices remain planned.
