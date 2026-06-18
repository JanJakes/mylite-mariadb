# Ownerless Compressed Row-Format Trace Export

## Problem Statement

Ownerless compressed row-format DDL has focused in-process coverage for
already-open peer refresh, key-block variants, and crash-at-dictionary-boundary
recovery. The deterministic external SQL trace suite still has no trace family
that exercises compressed row-format rebuilds while a concurrent reader polls
snapshot-visible state.

MyLite needs external-harness input for this DDL class so MariaDB-compatible
oracle runs and later RQG-style stress can include compressed table rebuilds
instead of relying only on embedded selectors.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` and `KEY_BLOCK_SIZE` table
  options for `CREATE TABLE` and `ALTER TABLE`.
- `mariadb/sql/sql_table.cc` preserves implicit row type across ALTER and marks
  explicit row-format ALTER options for handler processing.
- `mariadb/storage/innobase/handler/handler0alter.cc` treats explicit
  `ROW_FORMAT` and `KEY_BLOCK_SIZE` as rebuild-driving InnoDB ALTER options.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates compressed
  key-block values and maps compressed row-format options into native InnoDB
  table metadata.
- `information_schema.INNODB_SYS_TABLES.ROW_FORMAT` and
  `information_schema.TABLES.ROW_FORMAT` expose compressed metadata after a
  rebuild.
- `tools/ownerless-sql-trace-runner` consumes trace directories with
  `schema.sql`, concurrent worker/reader SQL files, `expected.sql`, and
  `manifest.txt`; `tools/ownerless-sql-trace-suite` registers deterministic
  trace families by name and validates them in `--check` mode.

## Design

Add `tools/ownerless-compressed-row-format-trace`, generating a deterministic
trace directory:

- `schema.sql` creates a stable aggregate table and a file-per-table InnoDB
  table with `ROW_FORMAT=DYNAMIC`, `LONGBLOB` payloads, and an index.
- `worker-1.sql` rebuilds the initially dynamic table through
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>` for a deterministic key-block
  cycle. It updates every payload row, advances a stable aggregate table, and
  checks compressed metadata after each round.
- `reader.sql` calls a stored reader procedure that repeatedly opens
  repeatable-read consistent snapshots and validates bounded row, version,
  payload, and stable aggregate values. The reader retries the whole snapshot
  block on MariaDB `1020`, `1205`, `1213`, `1412`, or SQLSTATE `40001`, which
  are ordinary external-server contention/table-definition-change outcomes for
  concurrent DDL/read replay.
- `expected.sql` verifies final compressed metadata, row aggregates, payload
  byte totals, final payload byte identity, and the stable aggregate oracle.
- `manifest.txt` records rounds, rows, payload size, key-block cycle,
  retry-limit, and expected totals.

Wire the trace into `tools/ownerless-sql-trace-suite` as
`compressed-row-format-ddl`, add a direct CTest check-mode smoke, and update the
compatibility matrix. The default CTest path remains dependency-free; external
MariaDB replay remains opt-in through the existing external smoke bridge.

## Scope

In scope:

- Deterministic SQL trace export for compressed row-format rebuilds.
- Trace-runner and trace-suite check-mode integration.
- Direct CTest smoke registration.
- Compatibility and spec documentation.

Out of scope:

- Product runtime changes.
- Ownerless embedded SQL selector changes.
- Crash injection during compressed rebuilds.
- Native `.ibd` compressed-page scanning in the external trace.
- Full external MariaDB/RQG long-running stress claims.

## Compatibility Impact

No MyLite SQL behavior changes. The slice expands external-harness coverage for
an already-supported partial ownerless DDL class by generating
MariaDB-compatible SQL that can run under the trace runner or an external
MariaDB/RQG-style harness.

## Database Directory And Lifecycle Impact

No MyLite database-directory layout changes. Generated trace files live under
the caller-provided output directory and target an external SQL server only
when replayed.

## Native Storage Impact

No MyLite native-storage code changes. The generated SQL exercises native
InnoDB `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE` rebuilds and validates metadata
through MariaDB information schema.

## Public API, Build, Size, And Dependencies

No public API, binary-size, license, or production dependency changes. The
slice adds one shell tool, one check-mode CTest entry, and trace-suite metadata.

## Test Plan

- Run `bash -n tools/ownerless-compressed-row-format-trace`.
- Run
  `tools/ownerless-compressed-row-format-trace --output DIR --rounds 3 --rows 2 --payload-bytes 12000 --check`.
- Run `tools/ownerless-sql-trace-runner --trace-dir DIR --check`.
- Run
  `tools/ownerless-sql-trace-suite --output DIR --trace compressed-row-format-ddl --check`.
- Run a focused CTest selector for the new tool and trace suite.
- Run `git diff --check`.

## Acceptance Criteria

- The exporter writes non-empty `schema.sql`, `worker-1.sql`, `reader.sql`,
  `expected.sql`, and `manifest.txt`.
- Generated SQL contains dynamic-to-compressed row-format ALTERs and a
  deterministic compressed key-block cycle.
- The reader uses repeatable-read consistent snapshots and bounded retries for
  ordinary external DDL/read contention.
- The final oracle verifies compressed metadata and deterministic final row
  aggregates.
- The trace runner accepts the generated package in `--check` mode.
- The full trace suite includes and can select `compressed-row-format-ddl`.
- Compatibility docs record the external trace input without claiming full
  external MariaDB/RQG completion.

## Risks And Follow-Up

- External replay depends on the chosen MariaDB server supporting compressed
  InnoDB tables. The default CTest path validates trace structure only.
- The trace validates metadata and SQL-level oracles, not native compressed page
  classes; embedded ownerless selectors remain the page-evidence authority.
- Full external MariaDB/RQG long-running compressed DDL stress remains planned.
