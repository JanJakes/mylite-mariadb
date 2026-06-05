# Ownerless External Full Scale 2 Replay

## Problem Statement

Ownerless external trace coverage can replay all deterministic trace families
against a disposable MariaDB server at scale 1, and can replay the
active-reader/BLOB pressure subset at scale 2. The remaining external
MariaDB/RQG gap is still broader than one slice, but MyLite can raise the
bounded real-client evidence by replaying the full deterministic suite at
scale 2.

This slice records full-suite deterministic external replay evidence at scale 2
without claiming long-running randomized RQG coverage.

## Source Findings

- `tools/ownerless-sql-trace-suite` owns the ordered deterministic trace list:
  independent-table stress, random transaction stress, FK graph stress, DDL
  stress, DDL lifecycle, checksum stress, transaction/savepoint stress,
  temporary-table stress, active-reader pressure, and BLOB pressure.
- `tools/ownerless-sql-trace-suite` accepts `--scale N` in the bounded range
  1 through 25 and multiplies each trace family's smoke rounds, row counts, or
  reader polls while leaving BLOB payload bytes fixed.
- `tools/ownerless-external-mariadb-trace-smoke` starts a disposable
  `mariadb:11.8` Docker container and passes `--scale` and any selected traces
  to `tools/ownerless-sql-trace-suite`.
- `docs/specs/ownerless-pressure-external-replay-evidence/specs.md` records
  scale-2 external replay for active-reader/BLOB pressure and full scale-1
  external replay for all 10 deterministic traces.

## Design

Run the existing Docker-backed smoke tool with no trace filter and
`--scale 2`:

```sh
tools/ownerless-external-mariadb-trace-smoke \
  --output /tmp/mylite-ownerless-external-full-scale2-replay \
  --scale 2
```

If the replay passes, document the evidence in this spec, the ownerless
cross-process concurrency spec, and the compatibility matrix. If a trace fails
with ordinary retryable MariaDB contention, first inspect the generated SQL and
trace log before changing retry contracts. If a trace exposes a deterministic
oracle bug, fix the exporter and rerun the full scale-2 replay.

The first full scale-2 replay attempt exposed a retry-contract gap in the
active-reader trace reader. MariaDB returned `ERROR 1020 (HY000)` while the raw
reader transaction was taking its initial aggregate snapshot under concurrent
updates. The fix keeps the trace semantics unchanged but moves the reader body
into a bounded-retry stored procedure that catches `1020`, `1205`, `1213`, and
SQLSTATE `40001`, matching the retry-aware pressure trace pattern. The worker
procedure also treats SQLSTATE `40001` as retryable deadlock contention.

## Scope

In scope:

- Check-mode generation for the full deterministic trace suite at scale 2.
- Docker-backed MariaDB 11.8 replay of all 10 deterministic traces at scale 2.
- Bounded retry hardening for the active-reader trace reader when external
  MariaDB reports ordinary `1020`, `1205`, `1213`, or SQLSTATE `40001`
  contention.
- Documentation and compatibility evidence updates.

Out of scope:

- Adding Docker replay to default CI.
- Raising the scale beyond 2.
- Randomized RQG/SQLancer generation.
- Long-running external stress loops.
- Product runtime behavior changes.

## Compatibility Impact

No MyLite SQL behavior changes. The slice strengthens external compatibility
evidence by proving the deterministic traces can run at a higher bounded scale
through a real MariaDB client and server.

Full external MariaDB/RQG long-running randomized stress remains planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Generated traces and logs live
under the caller-provided output directory, and the Docker replay mutates only
the disposable external MariaDB container's `app` database.

## Native Storage Impact

No MyLite native-storage format changes.

## Public API, Build, Size, And Dependencies

No public API, build-profile, production binary-size, license, or dependency
changes. Docker remains opt-in for this evidence.

## Test Plan

- Run `tools/ownerless-sql-trace-suite --output DIR --scale 2 --check`.
- Run focused active-reader Docker replay after the retry fix:
  `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 2 --trace
  active-reader-pressure`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 2`.
- Inspect `suite-manifest.txt`, `external-manifest.txt`, and per-trace logs.
- Run `bash -n` over the affected shell tools if implementation changes are
  needed.
- Run focused tool CTests:
  `ctest --preset embedded-dev -R
  'tools\.ownerless-(active-reader-pressure-trace|sql-trace-suite-full-scaled|sql-trace-suite-pressure-scaled|external-mariadb-trace-smoke-check)'
  --output-on-failure`.
- Run `git diff --check`, cached diff checks, and cleanup checks.

## Acceptance Criteria

- Check-mode generation succeeds for all 10 deterministic traces at scale 2.
- Real Docker-backed MariaDB replay succeeds for all 10 deterministic traces at
  scale 2.
- Docs record the exact evidence without upgrading the full randomized external
  MariaDB/RQG stress status beyond planned.

## Evidence

The first full scale-2 Docker-backed replay failed in
`active-reader-pressure/reader.sql`:

```text
ERROR 1020 (HY000): Record has changed since last read in table
'ownerless_active_reader_trace'; try restarting transaction
```

After the active-reader reader retry hardening, focused scale-2 replay of
`active-reader-pressure` passed:

```text
scale=2
trace_count=1
trace=active-reader-pressure
suite_run=ok
external_mariadb_trace_smoke=ok
```

The full scale-2 Docker-backed MariaDB 11.8 replay then passed all 10
deterministic traces:

```text
scale=2
trace_count=10
trace=independent-table-stress
trace=random-tx
trace=fk-graph
trace=ddl-stress
trace=ddl-lifecycle
trace=checksum-stress
trace=transaction-stress
trace=temporary-table-stress
trace=active-reader-pressure
trace=blob-pressure
suite_run=ok
external_mariadb_trace_smoke=ok
```

## Risks And Unresolved Questions

- Full scale-2 deterministic replay is still not randomized RQG.
- Docker image availability and host load can affect wall time, so the evidence
  should record trace success and manifest details rather than treating runtime
  as a product performance metric.
