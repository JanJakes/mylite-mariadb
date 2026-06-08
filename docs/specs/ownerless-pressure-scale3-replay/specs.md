# Ownerless Pressure Scale 3 Replay

## Problem Statement

Ownerless active-reader and BLOB pressure traces have default check-mode
coverage and Docker-backed MariaDB 11.8 replay evidence at scale `2`. The
remaining pressure gap is still long-running external MariaDB/RQG stress, but a
scale `3` deterministic pressure profile is cheap enough to validate in CTest
and gives stronger bounded real-client evidence for the active-reader pressure
families that retain page-version WAL under a snapshot pin.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-sql-trace-suite` accepts repeated `--trace` filters and a
  bounded `--scale` value.
- `tools/ownerless-active-reader-pressure-trace` emits a retry-aware
  repeatable-read snapshot reader, a deterministic writer, replacement-copy
  DDL pressure, and AUTO_INCREMENT high-watermark oracles.
- `tools/ownerless-blob-pressure-trace` emits retry-aware dynamic and
  compressed BLOB pressure SQL plus final aggregate oracles.
- Existing CTest coverage registers
  `tools.ownerless-sql-trace-suite-pressure-scaled` at scale `2`.

## Scope And Non-Goals

In scope:

- Add `tools.ownerless-sql-trace-suite-pressure-wide-scaled`, a
  dependency-free CTest check for active-reader and BLOB pressure traces at
  scale `3`.
- Record opt-in Docker-backed MariaDB 11.8 replay evidence for the same
  two-trace scale `3` profile.
- Update compatibility and ownerless cross-process documentation.

Out of scope:

- Making Docker replay a default CI dependency.
- Adding random SQL generation, RQG, SQLancer, shrinking, or long-running
  external stress loops.
- Changing MyLite runtime behavior, SQL compatibility, storage layout, public
  APIs, or directory lifecycle behavior.

## Design

Keep the existing scale `2` check and register a second pressure profile:

```sh
tools/ownerless-sql-trace-suite \
  --output <ctest-binary-dir>/ownerless-sql-trace-suite-pressure-wide-scaled-smoke \
  --trace active-reader-pressure \
  --trace blob-pressure \
  --scale 3 \
  --check
```

At scale `3`, the active-reader pressure trace expands to 12 rounds, 12 rows,
and 24 reader polls. The BLOB pressure trace expands to 9 rounds, 6 rows,
12,000-byte payloads, and 18 reader polls. Check mode validates generated SQL,
manifests, and final oracle SQL without Docker.

The matching Docker replay remains manual evidence:

```sh
tools/ownerless-external-mariadb-trace-smoke \
  --output build/manual-external-pressure-scale3-replay \
  --scale 3 \
  --trace active-reader-pressure \
  --trace blob-pressure
```

## Compatibility Impact

No product behavior changes. This slice broadens deterministic pressure trace
evidence and keeps full randomized external MariaDB/RQG pressure stress marked
planned.

## Directory And Lifecycle Impact

No MyLite database-directory behavior changes. Check-mode artifacts stay under
the CTest build directory. Optional replay writes generated traces and logs
under the requested output directory and mutates only the disposable external
MariaDB container's `app` database.

## Native Storage Impact

No native storage format changes. Optional replay exercises unmodified MariaDB
11.8 native InnoDB through the existing trace-runner contract.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes.
Default CTest coverage remains dependency-free and does not require Docker.

## Verification Plan

- Run the direct scale `3` pressure trace-suite check.
- Reconfigure `prod` and run focused CTest for the scale `2` and scale `3`
  pressure checks.
- If Docker is available, replay active-reader and BLOB pressure traces at
  scale `3` through `tools/ownerless-external-mariadb-trace-smoke`.
- Inspect replay manifests and final oracle logs.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Default CTest registers and passes
  `tools.ownerless-sql-trace-suite-pressure-wide-scaled`.
- Existing scale `2` pressure check continues to pass.
- Scale `3` check mode validates both selected pressure traces.
- Docker-backed MariaDB 11.8 scale `3` replay passes when Docker is available.
- Docs continue to distinguish deterministic pressure replay from full
  randomized external MariaDB/RQG stress.

## Evidence

The direct dependency-free scale `3` check passed:

```text
tools/ownerless-sql-trace-suite --output build/manual-pressure-scale3-check-probe --trace active-reader-pressure --trace blob-pressure --scale 3 --check
active-reader-pressure: rounds=12 rows=12 reader_polls=24 expected_versions=12
blob-pressure: rounds=9 rows=6 payload_bytes=12000 reader_polls=18 expected_versions=54
trace_count=2
suite_check=ok
elapsed_seconds=2
```

Docker-backed MariaDB 11.8 replay passed for the same two-trace scale `3`
profile:

```text
tools/ownerless-external-mariadb-trace-smoke --output build/manual-external-pressure-scale3-replay --scale 3 --trace active-reader-pressure --trace blob-pressure
trace=active-reader-pressure
trace=blob-pressure
scale=3
trace_count=2
suite_run=ok
external_mariadb_trace_smoke=ok
```

The active-reader final oracle reported rows `12`, sum `858`, versions `12`,
payload bytes `48000`, replacement-copy checks `ok`, and AUTO_INCREMENT
high-watermark state `rows=4`, `id_sum=106`, `max_id=100`, `value_sum=1060`.
The dynamic and compressed BLOB final oracles both reported rows `6`, sum
`276`, versions `54`, payload bytes `72000`, first-byte sum `636`, and final
checks `ok`. All final `expected.err` files were empty, and the disposable
Docker container was removed.

After reconfiguring `prod`, focused CTest coverage passed the existing scale
`2` and new scale `3` pressure checks:

```text
ctest --preset prod -R 'tools\.ownerless-sql-trace-suite-pressure.*scaled$' --output-on-failure
tools.ownerless-sql-trace-suite-pressure-scaled ... Passed
tools.ownerless-sql-trace-suite-pressure-wide-scaled ... Passed
100% tests passed, 0 tests failed out of 2
```

## Risks And Follow-Up

- Scale `3` remains bounded deterministic replay; it does not replace
  long-running randomized RQG/SQLancer execution.
- Future trace growth could make the scale `3` check too expensive for default
  CTest. If that happens, keep scale `2` as default and move scale `3` to an
  opt-in profile.
