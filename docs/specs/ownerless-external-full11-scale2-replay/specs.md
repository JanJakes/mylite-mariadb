# Ownerless External Full 11 Scale 2 Replay

## Problem Statement

The deterministic ownerless SQL trace suite grew from 10 to 11 families after
adding CTAS post-create DML. The previous full scale-2 Docker-backed MariaDB
replay covered the 10-family suite that existed at that time, and focused CTAS
DML replay covered the new trace alone. The remaining bounded external evidence
gap is proving the current full deterministic suite together at scale 2.

This slice records a full 11-family deterministic external MariaDB replay at
scale 2 without claiming long-running randomized RQG coverage.

## Source Findings

- `tools/ownerless-sql-trace-suite` now emits 11 deterministic trace families:
  independent-table stress, random transaction stress, FK graph stress, DDL
  stress, DDL lifecycle, CTAS DML, checksum stress,
  transaction/savepoint stress, temporary-table stress, active-reader pressure,
  and BLOB pressure.
- `tools/ownerless-external-mariadb-trace-smoke` starts a disposable
  `mariadb:11.8` Docker container and replays the generated trace suite through
  the real `mariadb` client.
- The previous `ownerless-external-full-scale2-replay` evidence remains valid
  historical evidence for the earlier 10-family suite. The CTAS trace-export
  slice added focused CTAS DML external replay evidence, but not a full current
  suite replay.

## Design

Run the external smoke tool without a trace filter at scale 2:

```sh
tools/ownerless-external-mariadb-trace-smoke \
  --output /tmp/mylite-ownerless-external-full11-scale2-replay \
  --scale 2
```

Record the suite manifest, final external smoke result, and final oracle
results. Keep randomized MariaDB/RQG stress separate from this bounded
deterministic evidence.

## Scope

In scope:

- Full current deterministic trace-suite replay at scale 2 through MariaDB
  11.8 Docker.
- Documentation and compatibility evidence updates.
- Cleanup of temporary replay directories.

Out of scope:

- Default CI Docker replay.
- Raising the scale beyond 2.
- Randomized RQG/SQLancer generation.
- Product runtime behavior changes.

## Compatibility Impact

No MyLite SQL behavior changes. The slice strengthens external compatibility
evidence by proving the current deterministic trace suite can run through a
real MariaDB 11.8 client/server at scale 2.

Full randomized external MariaDB/RQG stress remains planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. The replay writes traces and logs
under the requested output directory and mutates only the disposable external
MariaDB container's `app` database.

## Native Storage Impact

No MyLite native-storage format changes.

## Public API, Build, Size, And Dependencies

No public API, build-profile, production binary-size, license, or dependency
changes. Docker remains opt-in for this evidence.

## Test Plan

- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 2`.
- Inspect `suite-manifest.txt`, `external-manifest.txt`, per-trace final
  oracle output, and stderr files.
- Run dependency-free trace CTests and static checks when docs are updated.
- Run `git diff --check`, cached diff checks, and cleanup checks.

## Acceptance Criteria

- Real Docker-backed MariaDB replay succeeds for all 11 deterministic traces at
  scale 2.
- Final per-trace oracles report `ok`.
- Docs record the evidence without upgrading randomized external MariaDB/RQG
  stress beyond planned.

## Evidence

The full scale-2 Docker-backed MariaDB 11.8 replay passed all 11 deterministic
traces:

```text
scale=2
trace_count=11
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
trace=blob-pressure
suite_run=ok
external_mariadb_trace_smoke=ok
```

All final `expected.err` files were empty. The FK graph trace also emitted
nonzero `negative-worker-*-expected-error-*.err` files, which are the expected
negative-oracle failures for invalid FK operations.

Final oracle logs reported `ok` for all trace families:

```text
ownerless_independent_stress_trace_check ok
ownerless_random_tx_trace_check ok
ownerless_fk_graph_trace_check ok
ownerless_ddl_stress_trace_check ok
ownerless_ddl_lifecycle_trace_check ok
ownerless_ctas_dml_trace_check ok
ownerless_checksum_stress_trace_check ok
ownerless_tx_stress_trace_check ok
ownerless_temp_stress_trace_check ok
ownerless_active_reader_trace_check ok
dynamic_blob_final_check ok
compressed_blob_final_check ok
```

A later current-suite rerun after adding the DDL lifecycle same-name recreate
`INNODB_SYS_TABLES.SPACE` oracle also passed all 11 deterministic traces at
scale 2:

```text
scale=2
trace_count=11
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
trace=blob-pressure
suite_run=ok
external_mariadb_trace_smoke=ok
```

The updated `ddl-lifecycle` final oracle reported `observed_space_rows=8`,
`observed_valid_space_rows=8`, `observed_recreated_space_changes=8`, and
matching final dictionary/recreated space ids. All positive final `expected.err`
files were empty; the only non-empty stderr files were the expected FK graph
negative-oracle errors.

A current-suite rerun on 2026-06-08 after adding the active-reader
AUTO_INCREMENT high-watermark oracle also passed all 11 deterministic traces at
scale 2:

```text
scale=2
trace_count=11
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
trace=blob-pressure
suite_run=ok
external_mariadb_trace_smoke=ok
```

The active-reader pressure final oracle included the new AUTO_INCREMENT state:

```text
observed_auto_inc_rows=4
observed_auto_inc_id_sum=106
observed_auto_inc_max_id=100
observed_auto_inc_value_sum=1060
ownerless_active_reader_auto_inc_check=ok
```

All positive final `expected.err` files were empty. The only non-empty stderr
files were the expected FK graph negative-oracle errors.

Local follow-up verification for the documentation update also passed:

```text
ctest --preset prod -R 'tools\.ownerless-(sql-trace-suite-full-scaled|external-mariadb-trace-smoke-check)$' --output-on-failure
cmake --build --preset format-check-prod
git diff --check
```

## Risks And Unresolved Questions

- Full deterministic scale-2 replay is still not randomized RQG.
- Docker image availability and host load can affect wall time, so this evidence
  records trace success and oracle details rather than treating runtime as a
  product performance metric.
