# Ownerless SQL Trace Suite

## Problem Statement

Ownerless concurrency now has deterministic SQL trace exporters for the major
stress families, and `tools/ownerless-sql-trace-runner` can validate or replay
one trace directory at a time. Full external MariaDB/RQG-style oracle execution
still remains outside the default CI environment, but users need one stable
suite entry point that generates and validates every exported trace shape before
running them against an external MariaDB-compatible client.

## Source Findings

- `tools/ownerless-sql-trace-runner` accepts a trace directory, validates the
  required files with `--check`, and can replay the trace through a supplied SQL
  client with repeated `--client-arg` options.
- Existing deterministic exporters cover independent-table stress, random
  transaction stress, foreign-key graph stress, DDL/DML stress, DDL lifecycle
  stress, checksum stress, explicit transaction/savepoint stress, temporary
  table stress, and active-reader pressure.
- Existing CMake coverage runs each exporter as a smoke test but does not
  validate the generated trace directories through the common runner as one
  external replay suite.

## Design

Add `tools/ownerless-sql-trace-suite`:

1. Generate every deterministic ownerless trace family into subdirectories under
   one suite output directory using bounded smoke-sized parameters.
2. Validate each generated trace directory with
   `tools/ownerless-sql-trace-runner --check` when the suite is run with
   `--check` or `--dry-run`.
3. Replay each generated trace through the trace runner when a caller supplies a
   MariaDB-compatible client and optional `--client-arg` values.
4. Write `suite-manifest.txt` with the ordered trace list.
5. Register a CMake smoke test that generates and validates the full suite in
   check mode.

## Scope

In scope:

- Deterministic full-suite trace generation.
- Trace-runner validation for every generated trace.
- Optional external SQL client replay through the existing runner interface.
- Documentation and compatibility matrix updates.

Out of scope:

- Starting or managing a MariaDB daemon.
- RQG random workload generation.
- Claiming external oracle execution in CI.
- Changing product ownerless concurrency behavior.

## Compatibility Impact

No product SQL behavior changes. The suite improves compatibility evidence by
making the deterministic external replay package reproducible and easy to run
against a throwaway MariaDB-compatible environment.

## Directory And Lifecycle Impact

No MyLite database directory layout changes. Generated suite directories contain
only SQL traces and logs. External replay should target a throwaway server or
schema because traces create and mutate an `app` database.

## Native Storage Impact

No storage format changes.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds a shell tool and CMake smoke
test.

## Test Plan

- Run `bash -n tools/ownerless-sql-trace-suite`.
- Run `tools/ownerless-sql-trace-suite --output DIR --check`.
- Run the focused CMake smoke test with
  `ctest --preset embedded-dev -R 'tools\\.ownerless-sql-trace-suite'`.
- Run the broader ownerless trace tool filter.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- The suite generates every deterministic ownerless trace family.
- The suite validates each generated trace through
  `ownerless-sql-trace-runner --check`.
- The suite can pass `--client`, repeated `--client-arg`, `--log-dir`, and
  `--skip-negative` through to the trace runner for external replay.
- The CMake smoke test covers check-mode suite generation and validation.

## Risks And Open Questions

- This does not prove real external MariaDB/RQG execution; it only provides the
  deterministic input and runner bridge.
- External replay reuses the trace exporters' `app` database naming. Callers
  should use a disposable external server or schema namespace.
