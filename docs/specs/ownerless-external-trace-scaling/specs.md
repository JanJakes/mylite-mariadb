# Ownerless External Trace Scaling

## Problem Statement

The deterministic external MariaDB trace smoke now replays every generated trace
once with small fixed parameters. That is useful as a real-client bridge, but it
does not give developers a supported way to raise pressure for one trace family
or run a bounded subset before moving to full external MariaDB/RQG stress.

Full randomized RQG remains a separate environment-owned follow-up. This slice
adds a deterministic scale knob and trace selection to the existing tools so
external validation can grow from smoke to a repeatable stress probe without
editing scripts.

## Source Findings

- `tools/ownerless-sql-trace-suite` already owns the ordered deterministic
  trace list and hard-codes smoke-sized exporter parameters.
- The individual exporters already accept bounded `--rounds`, `--iterations`,
  `--rows`, or `--reader-polls` values:
  - DDL stress allows up to 200 rounds.
  - DDL lifecycle allows up to 100 rounds.
  - Independent-table stress allows up to 10,000 iterations and 20,000 reader
    polls.
  - Random transaction, checksum, transaction, temporary-table, FK graph,
    active-reader, and BLOB pressure exporters all have larger bounds than the
    current smoke defaults.
- `tools/ownerless-external-mariadb-trace-smoke` currently forwards
  `--skip-trace` but not positive `--trace` selection, so a caller cannot run a
  focused external replay without skipping every other trace manually.
- `tools/ownerless-sql-trace-runner` already records per-trace logs when
  `--log-dir` is supplied, so scaled replay evidence remains inspectable.

## Design

Add `--scale N` to `tools/ownerless-sql-trace-suite`:

- Valid values are `1` through `25`.
- The default remains `1`.
- The suite multiplies existing smoke-sized rounds, iterations, rows, and reader
  polls by the scale.
- BLOB payload byte size remains fixed at 12,000 bytes so scaled runs increase
  operation count and row count without unexpectedly allocating very large
  payloads.
- `suite-manifest.txt` and stdout include `scale=N`.

Add `--scale N` and repeatable `--trace NAME` to
`tools/ownerless-external-mariadb-trace-smoke`:

- The Docker wrapper validates scale with the same bounds and passes it to the
  suite.
- Requested traces are forwarded to the suite.
- Check-mode and run-mode manifests include scale and requested trace names.

Add dependency-free CTest coverage for scaled trace generation by running the
random transaction trace at scale 2 in check mode, and extend the external smoke
check plan with the same scale/trace options.

## Scope

In scope:

- Deterministic scaling for all existing trace families.
- Focused external Docker replay through `--trace`.
- Check-mode coverage for scaled generation/planning.
- Documentation and compatibility evidence updates.

Out of scope:

- Randomized RQG generation.
- Long-running CI-owned external stress.
- Changing ownerless product runtime behavior.
- Adding new SQL trace families.

## Compatibility Impact

No MyLite SQL behavior changes. The slice improves external compatibility
evidence by making deterministic replay intensity configurable and reproducible.

## Directory And Lifecycle Impact

No MyLite database directory changes. Generated SQL traces and logs remain under
caller-specified output directories. Docker replay still mutates only the
disposable external container's `app` database.

## Native Storage Impact

No native storage format changes.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. This changes shell tooling, CTest metadata,
and docs only.

## Test Plan

- Run `bash -n tools/ownerless-sql-trace-suite`.
- Run `bash -n tools/ownerless-external-mariadb-trace-smoke`.
- Run `tools/ownerless-sql-trace-suite --output DIR --trace random-tx --scale 2
  --check`.
- Run `tools/ownerless-external-mariadb-trace-smoke --output DIR --scale 2
  --trace random-tx --check`.
- If Docker is available, run the focused external replay with `--scale 2
  --trace random-tx`.
- Run focused CTests for the scaled suite and external check.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- The suite rejects invalid scale values.
- Scaled check-mode generation writes `scale=N` in the suite manifest.
- The external smoke wrapper forwards requested traces and scale to the suite.
- Focused Docker replay can run one scaled trace without running the full suite.
- Docs still mark full external MariaDB/RQG long-running randomized stress as
  planned.
