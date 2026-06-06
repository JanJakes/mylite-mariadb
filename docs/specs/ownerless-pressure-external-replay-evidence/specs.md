# Ownerless Pressure External Replay Evidence

## Problem Statement

Ownerless active-reader and BLOB pressure traces already provide deterministic
external-harness SQL, but the compatibility matrix still describes full external
MariaDB/RQG pressure stress as planned. Long-running randomized RQG remains
environment-owned, but the branch can record bounded real-client evidence for
the highest-pressure deterministic trace families.

## Source Findings

- `tools/ownerless-sql-trace-suite` accepts repeatable `--trace` filters and a
  bounded `--scale` value from 1 through 25.
- `tools/ownerless-external-mariadb-trace-smoke` starts a disposable MariaDB
  11.8 Docker container and replays selected deterministic traces through the
  real `mariadb` client.
- A full scale-1 replay on 2026-06-04 exposed ordinary MariaDB `1213` deadlock
  contention in the BLOB pressure worker and `1020` snapshot retry contention in
  the BLOB pressure reader, so that trace needs the same bounded retry contract
  already used by active-reader and FK graph traces.
- The same full replay exposed MariaDB deadlock reporting as SQLSTATE `40001`
  in the FK graph worker procedures, so deterministic external traces must treat
  numeric `1213` and SQLSTATE `40001` as the same retryable deadlock class.
- Default CTest currently validates the full trace suite in check mode and a
  scaled `random-tx` trace, but it does not have a focused scaled check for the
  pressure trace families.

## Design

Add a dependency-free CTest check named
`tools.ownerless-sql-trace-suite-pressure-scaled` that runs:

```sh
tools/ownerless-sql-trace-suite \
  --output <build>/ownerless-sql-trace-suite-pressure-scaled-smoke \
  --trace active-reader-pressure \
  --trace blob-pressure \
  --scale 2 \
  --check
```

Record opt-in Docker-backed replay evidence for the same trace subset:

```sh
tools/ownerless-external-mariadb-trace-smoke \
  --output /tmp/mylite-ownerless-external-pressure-replay \
  --scale 2 \
  --trace active-reader-pressure \
  --trace blob-pressure
```

The focused replay validates the active-reader pressure trace with 8 rounds,
8 rows, and 16 reader polls, plus the BLOB pressure trace with 6 rounds, 4 rows,
12,000-byte payloads, and 12 reader polls. The BLOB trace covers both dynamic
and compressed BLOB tables through its retry-aware worker, retry-aware reader,
and final oracle.

## Scope

In scope:

- Dependency-free scaled trace-generation CTest coverage for active-reader and
  BLOB pressure traces.
- Recorded focused Docker-backed MariaDB 11.8 replay evidence for the same
  scaled traces.
- Bounded retry in the BLOB pressure worker for MariaDB `1205`/`1213` plus
  SQLSTATE `40001` contention, and in the BLOB pressure reader for `1020`,
  `1205`, `1213`, and SQLSTATE `40001` contention, during raw-client external
  replay.
- Bounded SQLSTATE `40001` retry handling in the FK graph trace procedures when
  MariaDB reports deadlocks through SQLSTATE instead of only numeric errno
  matching.
- Recorded full scale-1 Docker-backed MariaDB 11.8 replay evidence for the
  deterministic trace suite.
- Compatibility and ownerless concurrency documentation updates.

Out of scope:

- Making Docker replay a default CI dependency.
- Randomized RQG generation.
- Long-running external stress loops.
- Product runtime behavior changes.

## Compatibility Impact

No product SQL behavior changes. The slice improves external compatibility
evidence for deterministic pressure traces and makes the generated BLOB
worker/reader plus FK graph workers robust to ordinary MariaDB retryable
outcomes while still marking full randomized external MariaDB/RQG pressure
stress as planned.

## Directory And Lifecycle Impact

No MyLite database directory layout changes. The new CTest writes generated SQL
under the build tree. The opt-in Docker replay writes traces and logs under the
requested output directory and mutates only the disposable external MariaDB
container's `app` database.

## Native Storage Impact

No MyLite native storage format changes.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds one CTest registration and
documentation.

## Test Plan

- Run `tools/ownerless-sql-trace-suite --output DIR --trace
  active-reader-pressure --trace blob-pressure --scale 2 --check`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 2
  --trace active-reader-pressure --trace blob-pressure`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 1`
  after the BLOB retry fix.
- Run focused Docker-backed external replay for `blob-pressure` and `fk-graph`
  after changing their retry contracts.
- Run the focused CTest for `tools.ownerless-sql-trace-suite-pressure-scaled`.
- Run the full dev CTest suite, format-check, tidy, `git diff --check`, and
  cleanup checks.

## Evidence

On 2026-06-04, the focused Docker-backed replay against the local
`mariadb:11.8` image passed with:

```text
trace=active-reader-pressure
trace=blob-pressure
scale=2
trace_count=2
suite_run=ok
external_mariadb_trace_smoke=ok
```

The trace parameters were:

```text
active-reader-pressure: rounds=8 rows=8 reader_polls=16 expected_versions=8
blob-pressure: rounds=6 rows=4 payload_bytes=12000 reader_polls=12 expected_versions=24
```

After hardening BLOB pressure and FK graph retry handling, the full scale-1
Docker-backed MariaDB replay passed for all 10 deterministic traces:
`independent-table`, `random-tx`, `fk-graph`, `ddl-stress`, `ddl-lifecycle`,
`checksum-stress`, `transaction-stress`, `temporary-table-stress`,
`active-reader-pressure`, and `blob-pressure`.

```text
scale=1
trace_count=10
suite_run=ok
external_mariadb_trace_smoke=ok
```

A later full-suite evidence slice raised the deterministic Docker-backed replay
to scale 2 for all 10 traces. That run first exposed MariaDB `1020` contention
in the active-reader raw reader transaction; after moving the active-reader
reader into a bounded-retry procedure, full scale-2 replay passed with
`trace_count=10`, `suite_run=ok`, and `external_mariadb_trace_smoke=ok`.
Those replay counts are historical evidence for the trace families present at
that time; later CTAS DML trace export raised the dependency-free check-mode
suite to 11 families without adding Docker replay evidence for the new family.

## Acceptance Criteria

- Check-mode generation for the selected pressure traces succeeds without
  Docker.
- Real Docker-backed MariaDB replay succeeds for the selected pressure traces at
  scale 2.
- Full scale-1 Docker-backed MariaDB replay succeeds after adding bounded BLOB
  worker/reader SQLSTATE `40001` retries and FK graph SQLSTATE `40001` retry
  handling.
- Docs record the evidence without upgrading the full randomized external
  MariaDB/RQG stress status beyond planned.
