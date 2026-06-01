# Ownerless Active-Reader Pressure Trace Export

## Problem Statement

Ownerless active-reader and expanding-page pressure are covered by embedded SQL
selectors and the opt-in `ownerless-stress` preset, but Phase 12 still calls out
external MariaDB/RQG oracle stress. Existing trace exporters make deterministic
writer, DDL, temporary-table, transaction, checksum, and foreign-key graph
schedules portable to external harnesses. The active-reader pressure shape has
not had the same portable trace package.

The bounded next step is a deterministic trace exporter that models a
repeatable-read snapshot reader while a worker mutates large InnoDB rows. This
does not replace long-running RQG, but it gives external runners a stable
package for the same pressure family.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines `MYSQL_START_TRANS_OPT_WITH_CONS_SNAPSHOT`
  for `START TRANSACTION WITH CONSISTENT SNAPSHOT`.
- `mariadb/sql/transaction.cc` routes start-transaction flags through
  `ha_start_consistent_snapshot()` when a consistent snapshot is requested.
- `mariadb/storage/innobase/handler/ha_innodb.cc` accepts
  `WITH CONSISTENT SNAPSHOT` only for repeatable-read-or-stronger InnoDB
  transactions and creates the snapshot through the InnoDB transaction.
- `tools/ownerless-sql-trace-runner` defines the current external trace
  package contract: `schema.sql`, concurrent `worker-*.sql` and `reader.sql`,
  optional `post.sql`, optional negative probes, `expected.sql`, and
  `manifest.txt`.
- `docs/specs/ownerless-active-reader-pressure/specs.md` and
  `docs/specs/ownerless-expanding-page-pressure/specs.md` define the
  ownerless product pressure shape: one repeatable-read snapshot pin with
  repeated or distinct large-row writers.

## Design

Add `tools/ownerless-active-reader-pressure-trace` with:

- `--output DIR`,
- `--rounds N` for deterministic writer updates,
- `--rows N` for the large-row table size,
- `--reader-polls N` for repeated snapshot-stability checks, and
- `--check` for dependency-free smoke validation.

The generated trace contains:

- `schema.sql`: creates `app.ownerless_active_reader_trace` with large
  `VARBINARY(4000)` payloads.
- `worker-1.sql`: updates rows in a deterministic cycle, increments a version
  counter, and emits periodic worker oracles.
- `reader.sql`: starts a repeatable-read consistent snapshot, records initial
  aggregate variables, repeatedly verifies the snapshot remains stable, then
  commits and verifies observed versions remain bounded by the final oracle.
- `expected.sql`: verifies final row count, value sum, version sum, and payload
  byte total.
- `manifest.txt`: records the constants and expected totals for external
  harnesses.

Add a CTest smoke entry beside the existing trace exporters.

## Scope

In scope:

- Deterministic active-reader pressure trace generation.
- Large-row updates that also cover expanding-page pressure input.
- Dependency-free local `--check` and CTest validation.
- Documentation and compatibility matrix updates for external harness input.

Out of scope:

- Running an external MariaDB server, RQG, SQLancer, or long-lived oracle stress
  in CI.
- Ownerless runtime or page-version WAL behavior changes.
- SQL-level table-lock wait fault injection.
- Timer-driven checkpoint scheduling.

## Compatibility Impact

No MyLite SQL semantics, public API, storage format, or runtime behavior
changes. The slice exports a MariaDB-compatible SQL schedule for external
comparison/oracle harnesses. Full external MariaDB/RQG long-running execution
remains planned and environment-owned.

## Directory And Lifecycle Impact

No MyLite database-directory layout changes. Generated trace files live under
the caller-provided output directory and target an external MariaDB-compatible
server only when replayed through `tools/ownerless-sql-trace-runner` or another
harness.

## Native Storage Impact

No native storage format changes. The trace uses ordinary InnoDB tables,
repeatable-read consistent snapshots, large `VARBINARY` rows, and deterministic
updates.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds one shell tool, one CTest
smoke, and documentation.

## Test Plan

- Run `tools/ownerless-active-reader-pressure-trace --rounds 4 --rows 3
  --reader-polls 6 --output <tmp> --check`.
- Validate that generated traces are accepted by
  `tools/ownerless-sql-trace-runner --trace-dir <tmp> --check`.
- Run `ctest --preset embedded-dev -R
  'tools.ownerless-(active-reader-pressure-trace|sql-trace-runner)'
  --output-on-failure`.
- Run the ownerless trace-tool CTest subset.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- The exporter generates non-empty schema, reader, worker, expected, and
  manifest files.
- The reader trace starts a repeatable-read consistent snapshot and includes
  snapshot-stability checks.
- The worker trace mutates deterministic large-row payloads and versions.
- The expected oracle verifies final count, value sum, version sum, and payload
  byte total.
- The trace-runner check accepts the generated package.

## Risks And Open Questions

- `tools/ownerless-sql-trace-runner` starts concurrent files but does not
  enforce a barrier that the reader snapshot begins before the worker's first
  update. The reader therefore records its own snapshot baseline and verifies
  stability from that point, while `expected.sql` verifies the durable final
  state after all concurrent files finish.
- This is deterministic external-harness input, not full RQG. Actual long-lived
  external MariaDB/RQG execution remains a follow-up.
