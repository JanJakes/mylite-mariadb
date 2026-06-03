# Ownerless BLOB Pressure Trace Export

## Problem

Ownerless active-reader pressure coverage now includes dynamic off-page BLOB
pages and compressed BLOB pages in the embedded cross-process SQL test. The
external trace suite still lacked a deterministic BLOB-pressure package, so
external MariaDB/RQG-style runners could replay ordinary row/update stress,
DDL stress, transaction stress, FK graph stress, and active-reader pressure,
but not the native BLOB pressure classes.

The bounded next step is a deterministic SQL trace exporter that carries both
`ROW_FORMAT=DYNAMIC` BLOB pressure and `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`
BLOB pressure through the existing trace-runner contract.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines native
  `FIL_PAGE_TYPE_BLOB`, `FIL_PAGE_TYPE_ZBLOB`, and `FIL_PAGE_TYPE_ZBLOB2`
  page classes for off-page BLOB storage.
- `mariadb/storage/innobase/btr/btr0cur.cc` stores long externally stored
  values on BLOB or compressed-BLOB pages.
- `docs/specs/ownerless-blob-page-pressure/specs.md` and
  `docs/specs/ownerless-compressed-blob-page-pressure/specs.md` already cover
  the ownerless in-process storage lifecycle and keep full external oracle
  pressure stress as a follow-up.
- `tools/ownerless-sql-trace-runner` accepts a trace directory containing
  `schema.sql`, concurrent worker/reader SQL files, `expected.sql`, and
  `manifest.txt`.
- `tools/ownerless-sql-trace-suite` generates every deterministic ownerless
  trace package and validates each with the trace runner in `--check` mode.

## Design

Add `tools/ownerless-blob-pressure-trace`. The exporter writes one trace
directory with:

- `schema.sql`: creates `app.ownerless_blob_pressure_trace` with
  `ROW_FORMAT=DYNAMIC` and `app.ownerless_compressed_blob_pressure_trace` with
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`; both tables use `LONGBLOB`
  payloads.
- `worker-1.sql`: repeatedly updates every row in both tables, increasing
  `value`, incrementing `version`, and replacing the BLOB payload with a
  deterministic large value.
- `reader.sql`: starts `START TRANSACTION WITH CONSISTENT SNAPSHOT` and polls
  row count, value sum, version sum, payload byte total, and first-byte
  aggregate for both tables. Every poll must keep the original snapshot
  aggregates.
- `expected.sql`: verifies final row counts, value sums, version sums,
  payload byte totals, and first-byte aggregates for both tables.
- `manifest.txt`: records row, round, payload-size, and expected-oracle
  values for external harnesses.

Wire the exporter into `tools/ownerless-sql-trace-suite` as
`blob-pressure`, and add a direct CTest smoke entry in `tools/CMakeLists.txt`.

## Scope And Non-Goals

In scope:

- Deterministic SQL trace export for dynamic and compressed BLOB pressure.
- Trace-runner `--check` compatibility.
- Inclusion in the full deterministic trace suite.
- Docs and compatibility matrix updates that mark the new external-harness
  input while keeping full external MariaDB/RQG stress planned.

Out of scope:

- Production runtime changes.
- Prepared binary binding in the shell trace. The in-process compressed BLOB
  selector remains the evidence for prepared binary payload binding.
- Native `.ibd` page-type scanning during external replay.
- Starting or managing an external MariaDB server in default CI.
- Claiming full external MariaDB/RQG long-running pressure stress.

## Compatibility Impact

No MyLite SQL behavior changes. The slice expands deterministic external
harness input for ownerless pressure cases by producing MariaDB-compatible SQL
that can run under the existing trace runner and Docker smoke bridge.

## Directory And Lifecycle Impact

No MyLite directory layout changes. Generated trace files live in the
caller-provided output directory and target an external MariaDB-compatible
server only when replayed through `tools/ownerless-sql-trace-runner` or another
external harness.

## Native Storage Impact

No MyLite native storage changes. The generated SQL uses native InnoDB
`ROW_FORMAT=DYNAMIC` and `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` tables so
external replay can exercise the same BLOB storage classes at SQL level.

## Public API, Build, And Dependency Impact

No public API or production dependency changes. The slice adds one shell tool,
one CTest smoke entry, and documentation.

## Test Plan

- Run `bash -n tools/ownerless-blob-pressure-trace`.
- Run
  `tools/ownerless-blob-pressure-trace --output DIR --rounds 3 --rows 2 --payload-bytes 12000 --reader-polls 6 --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir DIR --check` over that
  generated trace.
- Run
  `tools/ownerless-sql-trace-suite --output DIR --trace blob-pressure --check`.
- Run
  `ctest --preset embedded-dev -R 'tools\\.ownerless-(blob-pressure-trace|sql-trace-suite|sql-trace-runner|external-mariadb-trace-smoke-check)'`.
- Run `git diff --check`.

## Acceptance Criteria

- The exporter creates non-empty `schema.sql`, `worker-1.sql`, `reader.sql`,
  `expected.sql`, and `manifest.txt`.
- The generated schema includes both dynamic and compressed BLOB tables.
- The reader trace uses a consistent snapshot and validates stable BLOB
  aggregates for both tables.
- The expected trace validates final aggregates for both tables.
- The trace runner accepts the generated package in `--check` mode.
- The full trace suite can select and validate `blob-pressure`.
- Compatibility docs record the new deterministic trace input without claiming
  full external MariaDB/RQG completion.

## Risks And Follow-Up

- SQL literals are deterministic and portable, but they are not a replacement
  for the in-process prepared binary compressed BLOB selector.
- External replay still depends on the chosen MariaDB server supporting
  compressed InnoDB tables. Default CI uses `--check`, not a daemon.
- Full external MariaDB/RQG long-running pressure stress remains planned.
