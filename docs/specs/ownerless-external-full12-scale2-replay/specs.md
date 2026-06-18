# Ownerless External Full 12 Scale 2 Replay

## Problem Statement

The deterministic ownerless SQL trace suite grew from 11 to 12 families after
adding compressed row-format DDL export. The previous full scale-2
Docker-backed MariaDB replay covered the 11-family suite that existed at that
time, and focused compressed row-format replay covered the new trace alone. The
remaining bounded evidence gap was proving the current full deterministic suite
together at scale 2.

The first current-suite replay exposed a CTAS reader retry-contract gap:
MariaDB can report `ERROR 1020` to a raw repeatable-read reader while another
connection is repeatedly rebuilding and mutating the CTAS destination. This
slice hardens that deterministic trace reader and records the full 12-family
MariaDB replay evidence without claiming randomized RQG coverage.

## Source Findings

- `tools/ownerless-sql-trace-suite` emits 12 deterministic trace families:
  independent-table stress, random transaction stress, FK graph stress, DDL
  stress, DDL lifecycle, CTAS DML, checksum stress, transaction/savepoint
  stress, temporary-table stress, active-reader pressure, compressed
  row-format DDL, and BLOB pressure.
- `tools/ownerless-external-mariadb-trace-smoke` starts a disposable
  `mariadb:11.8` Docker container and replays generated traces through the real
  `mariadb` client.
- `tools/ownerless-sql-trace-runner` scans positive trace outputs for
  `mismatch` and leaves expected FK graph negative-probe errors in their own
  `negative-worker-*-expected-error-*.err` files.
- Existing active-reader, compressed row-format, and BLOB pressure traces use
  bounded reader retries for ordinary raw-client DDL/DML contention outcomes.
  CTAS DML had the same concurrent-reader shape but still generated raw
  snapshot statements directly in `reader.sql`.

## Design

Move the CTAS DML reader body into a generated stored procedure in
`schema.sql`. The procedure preserves the existing monotonic aggregate oracle
but retries each repeatable-read snapshot block up to a bounded retry limit on
MariaDB `1020`, `1205`, `1213`, or SQLSTATE `40001`. `reader.sql` now calls the
procedure, and `manifest.txt` records the retry limit.

Then rerun focused CTAS DML replay and the full current deterministic suite:

```sh
tools/ownerless-external-mariadb-trace-smoke \
  --output build/manual-external-ctas-dml-retry-scale2 \
  --trace ctas-dml \
  --scale 2

tools/ownerless-external-mariadb-trace-smoke \
  --output build/manual-external-full12-scale2-current \
  --scale 2
```

Keep Docker replay opt-in and keep randomized MariaDB/RQG stress as a separate
gap.

## Scope

In scope:

- Bounded retry hardening for the deterministic CTAS DML external trace reader.
- Focused CTAS DML Docker-backed replay at scale 2.
- Full 12-family Docker-backed MariaDB 11.8 replay at scale 2.
- Documentation and compatibility evidence updates.

Out of scope:

- Product runtime behavior changes.
- Default CI Docker replay.
- Raising the deterministic replay scale beyond 2.
- Randomized RQG/SQLancer generation.

## Compatibility Impact

No MyLite SQL behavior changes. The slice strengthens external compatibility
evidence by proving the current deterministic trace suite can run through a
real MariaDB 11.8 client/server at scale 2.

Full randomized external MariaDB/RQG stress remains planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Generated traces and logs live
under the requested output directory and replay mutates only the disposable
external MariaDB container's `app` database.

## Native Storage Impact

No MyLite native-storage format changes.

## Public API, Build, Size, And Dependencies

No public API, build-profile, production binary-size, license, or dependency
changes. Docker remains opt-in for this evidence.

## Test Plan

- Run `bash -n` over the affected shell trace tools.
- Run `tools/ownerless-ctas-dml-trace --output DIR --rounds 3 --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir DIR --check`.
- Run `tools/ownerless-sql-trace-suite --output DIR --trace ctas-dml --scale 2
  --check`.
- Run focused CTAS DML Docker-backed replay at scale 2.
- Run full 12-family Docker-backed MariaDB replay at scale 2.
- Run focused CTest check-mode coverage, format checks, production-build guard,
  and `git diff --check`.

## Acceptance Criteria

- The CTAS DML reader uses bounded retries for ordinary raw-client
  `1020`/`1205`/`1213`/`40001` contention while preserving the final oracle.
- Focused CTAS DML external replay passes at scale 2.
- Full Docker-backed MariaDB replay succeeds for all 12 deterministic traces at
  scale 2.
- Final per-trace oracles report `ok`.
- Docs record the evidence without upgrading randomized external MariaDB/RQG
  stress beyond planned.

## Evidence

The first full 12-family scale-2 replay failed in `ctas-dml/reader.sql`:

```text
ERROR 1020 (HY000) at line 274: Record has changed since last read in table
'ownerless_sql'; try restarting transaction
```

After CTAS reader retry hardening, focused CTAS DML replay passed:

```text
scale=2
trace_count=1
trace=ctas-dml
suite_run=ok
external_mariadb_trace_smoke=ok
```

The CTAS manifest and final oracle reported:

```text
rounds=8
reader_polls=64
retry_limit=40
expected_rows=3
expected_id_sum=247
expected_value_sum=3021
expected_payload_bytes=12000
ownerless_ctas_dml_trace_check ok
ownerless_ctas_dml_trace_reader_retries 0
```

The full scale-2 Docker-backed MariaDB 11.8 replay then passed all 12
deterministic traces:

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

Final oracle logs reported `ok` for the positive checks:

```text
ownerless_independent_stress_trace_check ok
ownerless_random_tx_trace_check ok
ownerless_fk_graph_trace_check ok
ownerless_ddl_stress_trace_check ok
ownerless_ddl_lifecycle_trace_check ok
ownerless_ddl_lifecycle_space_trace_check ok
ownerless_ddl_lifecycle_replace_like_check ok
ownerless_ddl_lifecycle_replace_ctas_check ok
ownerless_ctas_dml_trace_check ok
ownerless_checksum_stress_trace_check ok
ownerless_tx_stress_trace_check ok
ownerless_temp_stress_trace_check ok
ownerless_active_reader_trace_check ok
ownerless_active_reader_replace_like_check ok
ownerless_active_reader_replace_ctas_check ok
ownerless_active_reader_auto_inc_check ok
ownerless_compressed_row_format_trace_check ok
dynamic_blob_final_check ok
compressed_blob_final_check ok
```

All positive final `*.err` files were empty. The only non-empty stderr files
were FK graph `negative-worker-*-expected-error-*.err` files, which are the
expected negative-oracle probes.

## Risks And Unresolved Questions

- Full deterministic scale-2 replay is still not randomized RQG.
- Docker image availability and host load can affect wall time, so this
  evidence records trace success and oracle details rather than treating runtime
  as a product performance metric.
