# Performance File Size Summary

## Problem

Prepared insert timing now separates step and commit cost, but the benchmark
still does not report final `.mylite` file size. Recent insert samples showed
that widening the append buffer mostly moves write work between step and
commit, while the larger product gap is write amplification from the current
one-row-per-page storage format.

Future packed-row and pager/WAL work needs a stable local baseline for final
file bytes and header page count so performance changes cannot be judged only
by shifted timing.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c::run_benchmark()` closes the database before
  deleting the temporary benchmark root, which is the right point to read
  durable file size.
- The public storage API already exposes `mylite_storage_open_header()` with
  page size and page count, avoiding private format headers in the benchmark
  tool.
- `MYLITE_PERF_KEEP_ROOT=1` can keep the temporary directory, but manual
  filesystem inspection should not be required for normal performance runs.

## Design

- After a successful benchmark run closes the database, `stat()` the primary
  `.mylite` file and read its public storage header.
- Print a separate `Database file` Markdown table with final bytes, header page
  size, and header page count.
- Keep timing rows and threshold semantics unchanged. The file summary is
  informational and does not add a `--max-us` metric.

## Affected Subsystems

- Local performance baseline tooling.
- Storage performance investigation documentation.

## Compatibility Impact

No SQL, API, storage-engine routing, or file-format behavior changes. This is
benchmark output only.

## Single-File And Lifecycle Impact

No lifecycle change. The summary is printed after close, so it reports the
durable primary file as left by a successful benchmark run.

## Binary-Size Impact

Benchmark-only code. No runtime library or dependency impact.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

## Acceptance Criteria

- Successful benchmark runs print final bytes, header page size, and header
  page count after the timing table.
- Existing timing output and threshold checks remain unchanged.
- The file summary uses public storage APIs rather than private format headers.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`:
  passed.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`:
  passed and printed:
  - `final bytes`: `45473792`
  - `header page size`: `4096`
  - `header page count`: `11102`

## Risks And Unresolved Questions

- The summary reports file-level size, not per-table logical payload bytes.
  Packed-row work may need richer per-table diagnostics later.
