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
3. Generate a retry-aware reader procedure plus `reader.sql` that repeatedly
   opens `START TRANSACTION WITH CONSISTENT SNAPSHOT`, reads the stable
   aggregate, verifies monotonic bounds, and commits. The reader retries
   bounded external MariaDB `1020`, `1205`, `1213`, and SQLSTATE `40001`
   contention.
4. Record each round's initial and same-name recreated InnoDB
   `INNODB_SYS_TABLES.SPACE` values in
   `app.ownerless_lifecycle_space_oracle`, proving the recreated table has a
   new native dictionary space id.
5. Generate `expected.sql` that verifies the final recreated table shape, row
   count, id/value/generation sums, payload bytes, stable aggregate total,
   absence of the moved table name, per-round space-id changes, and the final
   live dictionary space id.
6. Generate `manifest.txt` with deterministic oracle values.
7. Register a CMake smoke test with `--rounds 3 --check`.

## Scope

In scope:

- Deterministic SQL trace export for DDL lifecycle shapes.
- SQL-level InnoDB dictionary space-id oracle coverage for same-name recreate
  inside the DDL lifecycle trace.
- Bounded external-reader retry handling for ordinary MariaDB contention while
  the DDL lifecycle worker mutates the stable aggregate.
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
lifecycle behavior and records MariaDB dictionary `SPACE` identity changes for
same-name recreate.

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
- Run focused Docker-backed MariaDB replay for the updated DDL lifecycle trace
  when Docker is available.
- Run `bash -n`, `format-check`, `git diff --check`, cached diff checks, and
  cleanup checks.

## Acceptance Criteria

- The trace exporter produces non-empty `schema.sql`, `worker-1.sql`,
  `reader.sql`, `expected.sql`, and `manifest.txt`.
- The worker SQL contains rename, truncate, force rebuild, drop, and same-name
  recreate lifecycle operations.
- The reader SQL uses repeatable snapshot transactions.
- The generated reader procedure handles retryable external MariaDB contention
  without hiding final oracle failures.
- The expected oracle validates final recreated metadata and aggregate values.
- The expected oracle validates that each same-name recreate receives a
  nonzero `INNODB_SYS_TABLES.SPACE` value that differs from the dropped table's
  value, and that the final live dictionary space matches the last recreated
  table recorded by the worker.
- The trace runner accepts the generated trace plan with `--check`.

## Evidence

The first focused Docker-backed MariaDB 11.8 replay of the updated
`ddl-lifecycle` trace at scale 2 exposed raw-reader contention:

```text
ERROR 1020 (HY000): Record has changed since last read in table
'ownerless_sql'; try restarting transaction
```

After adding bounded reader retry handling, focused Docker-backed replay of the
updated trace passed at scale 2:

```text
scale=2
trace_count=1
trace=ddl-lifecycle
suite_run=ok
external_mariadb_trace_smoke=ok
```

The focused final oracle reported:

```text
observed_space_rows=8
observed_valid_space_rows=8
observed_recreated_space_changes=8
observed_final_space=41
expected_final_space=41
ownerless_ddl_lifecycle_trace_check=ok
ownerless_ddl_lifecycle_space_trace_check=ok
```

The full current 11-family deterministic Docker-backed MariaDB 11.8 replay at
scale 2 also passed after the DDL lifecycle space-oracle update, with
`trace_count=11`, `suite_run=ok`, and `external_mariadb_trace_smoke=ok`.
All positive final `expected.err` files were empty; FK graph negative-worker
expected-error stderr files were the expected negative-oracle outputs.

## Risks And Open Questions

- The trace now has focused and full-suite Docker-backed MariaDB 11.8 replay
  evidence, but it is still a deterministic schedule rather than randomized
  RQG/SQLancer generation.
- Deterministic traces complement but do not replace randomized DDL crash,
  recovery, and long-running external stress.
