# Ownerless DDL Lifecycle Trace Export

## Problem Statement

Ownerless DDL/file-lifecycle SQL coverage has grown through focused embedded
selectors for dropped, created, recreated, renamed, truncated, force-rebuilt,
multi-renamed, and schema-dropped InnoDB file-per-table final states. The
remaining completion bar still includes external MariaDB/RQG-style stress, but
the existing exported traces do not isolate the same DDL lifecycle shapes that
stress retained-WAL replay and final native file authority.

MyLite needs deterministic external-harness input for the DDL lifecycle class
so those shapes can be replayed outside the embedded C test binary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/ownerless-sql-trace-runner`
  - Consumes trace directories containing `schema.sql`, concurrent
    `worker-*.sql`/`ddl-worker-*.sql`/`dml-worker-*.sql`, optional
    `reader.sql`, optional `post.sql`, optional negative probes, `expected.sql`,
    and `manifest.txt`.
  - `--check` validates the trace plan without connecting to a database.
- `tools/ownerless-ddl-stress-trace`
  - Exports broader concurrent DDL/DML stress, but its final oracle expects no
    remaining DDL stress tables.
- Focused ownerless SQL selectors now cover no-live stale-reader final states
  for created and same-name recreated file-per-table tablespaces, but those
  selectors are not reusable by external MariaDB/RQG-style runners.

## Design

Add `tools/ownerless-ddl-lifecycle-trace`:

1. Generate `schema.sql` with `app.ownerless_sql` as a stable aggregate table.
2. Generate `worker-1.sql` that repeats a deterministic lifecycle sequence:
   create, insert, update, rename, truncate, force rebuild, drop, same-name
   recreate with a new `generation` column, insert, update, and aggregate
   mutation.
3. Generate `reader.sql` that repeatedly opens `START TRANSACTION WITH
   CONSISTENT SNAPSHOT`, reads the stable aggregate, verifies monotonic bounds,
   and commits.
4. Generate `expected.sql` that verifies the final recreated table shape, row
   count, id/value/generation sums, payload bytes, stable aggregate total, and
   absence of the moved table name.
5. Generate `manifest.txt` with deterministic oracle values.
6. Register a CMake smoke test with `--rounds 3 --check`.

## Scope

In scope:

- Deterministic SQL trace export for DDL lifecycle shapes.
- `ownerless-sql-trace-runner --check` compatibility.
- CMake smoke-test registration.
- Documentation and compatibility matrix updates.

Out of scope:

- Running against an external MariaDB daemon in CI.
- Proving crash recovery or retained-WAL replay directly; the embedded
  selectors remain the direct product coverage for those behaviors.
- SQL-level table-lock wait fault injection.

## Compatibility Impact

No product SQL behavior changes. The slice improves external compatibility
evidence by providing reusable MariaDB-compatible SQL traces for DDL lifecycle
stress.

## Directory And Lifecycle Impact

No MyLite database directory layout changes. The generated SQL exercises native
InnoDB `.frm`/`.ibd` lifecycle through a MariaDB-compatible client when an
external harness runs it.

## Native Storage Impact

No storage format changes. The trace targets native InnoDB file-per-table DDL
lifecycle behavior.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds a shell tool and CMake smoke
test.

## Test Plan

- Run `tools/ownerless-ddl-lifecycle-trace --rounds 3 --output DIR --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir DIR --check`.
- Run the CMake tool smoke test with
  `ctest --preset embedded-dev -R 'tools\\.ownerless-ddl-lifecycle-trace'`.
- Run the broader ownerless trace tool filter.
- Run `bash -n`, `format-check`, `git diff --check`, cached diff checks, and
  cleanup checks.

## Acceptance Criteria

- The trace exporter produces non-empty `schema.sql`, `worker-1.sql`,
  `reader.sql`, `expected.sql`, and `manifest.txt`.
- The worker SQL contains rename, truncate, force rebuild, drop, and same-name
  recreate lifecycle operations.
- The reader SQL uses repeatable snapshot transactions.
- The expected oracle validates final recreated metadata and aggregate values.
- The trace runner accepts the generated trace plan with `--check`.

## Risks And Open Questions

- This is external-harness input, not an external MariaDB result. Completion
  still requires running these traces under a real external MariaDB/RQG-style
  harness.
- Deterministic traces complement but do not replace randomized DDL crash and
  recovery stress.
