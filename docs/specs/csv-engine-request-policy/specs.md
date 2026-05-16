# CSV Engine Request Policy

## Problem

The native CSV engine is no longer registered in the default embedded profile,
but explicit application DDL can still ask for `ENGINE=CSV`. MyLite should not
let that request depend on MariaDB's missing-plugin path or accidentally route
it as a supported MyLite table shape.

## Design

- Add a narrow SQL policy check for `CREATE TABLE ... ENGINE=CSV` and
  `ALTER TABLE ... ENGINE=CSV`.
- Apply the policy before MariaDB execution for direct and prepared SQL.
- Keep quoted strings, comments, and ordinary column names from triggering the
  policy accidentally.
- Leave supported routed engine requests unchanged: omitted/default,
  `MYLITE`, `InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, and volatile
  `MEMORY` / `HEAP`.

## Compatibility Impact

`ENGINE=CSV` remains out of scope. Direct and prepared requests now fail with a
stable MyLite diagnostic before catalog publication. This does not implement
CSV table-file compatibility and does not affect ordinary CSV-shaped text data
stored in supported MyLite-routed tables.

## Test Plan

- Direct execution rejects `CREATE TABLE ... ENGINE=CSV`, temporary CSV tables,
  and `ALTER TABLE ... ENGINE=CSV`.
- Prepared execution rejects CSV engine requests before MariaDB prepares them.
- Storage-engine smoke verifies no MyLite catalog record is published and that
  an existing routed table keeps its original requested/effective engine
  metadata after a failed CSV engine change.
- Existing routed engine, sidecar, and server-surface groups continue to pass.

## Acceptance Criteria

- CSV engine requests return a MyLite-owned error with no MariaDB errno.
- Quoted text containing `ENGINE=CSV` does not trigger the policy.
- Failed CSV requests do not create or mutate MyLite catalog records.

## Implementation Verification

Completed on 2026-05-16:

- `cmake --build --preset embedded-dev`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface storage-engine sidecar`
- `cmake --build --preset dev`
- `ctest --preset dev --output-on-failure`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `git diff --check`
