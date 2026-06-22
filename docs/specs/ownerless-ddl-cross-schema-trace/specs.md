# Ownerless DDL Cross-Schema Trace

## Problem Statement

Ownerless DDL/file-lifecycle evidence already covers focused embedded recovery
selectors and a deterministic DDL lifecycle trace for create, rename,
truncate, force rebuild, drop, same-name recreate, and replacement-copy table
DDL. The exported trace did not yet carry cross-schema file movement or
schema-drop absence oracles, even though those are part of the broader
DDL/file-lifecycle completion bar.

Add deterministic external-harness SQL for those two lifecycle classes without
claiming full randomized MariaDB/RQG coverage.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:56` enters `mysql_rename_tables()` and takes
  metadata locks before calling the rename sequence.
- `mariadb/sql/sql_table.cc:5556` implements `mysql_rename_table()`, builds the
  old and new database-qualified paths, and calls `handler::ha_rename_table()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:14512` implements
  `ha_innobase::rename_table()` and routes to InnoDB dictionary rename while
  holding dictionary locks.
- `mariadb/sql/sql_db.cc:1068` starts `mysql_rm_db_internal()`, enumerates
  schema objects, locks table names, drops objects, and removes the schema
  directory.
- `mariadb/storage/innobase/handler/ha_innodb.cc:13899` implements
  `ha_innobase::delete_table()` for table removal, including DROP DATABASE
  table cleanup.
- `tools/ownerless-ddl-lifecycle-trace` already owns deterministic
  external-trace generation for the DDL lifecycle family and is registered in
  `tools/ownerless-sql-trace-suite`.

## Design

Extend `tools/ownerless-ddl-lifecycle-trace` rather than adding a separate
trace family so the lifecycle oracle remains a single deterministic external
input:

1. Create two auxiliary schemas:
   `ownerless_lifecycle_aux` for the retained cross-schema rename target and
   `ownerless_lifecycle_drop` for schema-drop file-lifecycle checks.
2. In each worker round, create `app.ownerless_lifecycle_xschema`, capture its
   InnoDB `INNODB_SYS_TABLES.SPACE`, rename it to
   `ownerless_lifecycle_aux.ownerless_lifecycle_xschema_moved`, update it, and
   verify that the moved table keeps the original `SPACE`.
3. In each worker round, create two InnoDB tables inside
   `ownerless_lifecycle_drop`, capture their `SPACE` values, drop the schema,
   recreate the schema empty for the next round, and record the captured spaces
   in `app.ownerless_lifecycle_xschema_oracle`.
4. Extend `expected.sql` to verify final moved-table rows, payload bytes,
   generation sum, secondary-index usability, source-name absence, per-round
   cross-schema `SPACE` identity, per-round dropped-table `SPACE` pairs, final
   moved-table `SPACE`, and empty dropped-schema metadata/dictionary state.
5. Extend manifest and dependency-free `--check` greps so trace-suite CTest
   catches accidental loss of these SQL shapes.

## Scope

In scope:

- Deterministic SQL trace export for cross-schema InnoDB table rename.
- Deterministic SQL trace export for schema drop of InnoDB tables.
- Final metadata/value/dictionary-space oracles for those shapes.
- Focused Docker-backed MariaDB replay of the updated trace.
- Compatibility/spec documentation updates.

Out of scope:

- Product storage-code changes.
- SQL-level table-lock fault injection.
- Claiming full randomized RQG or long-running external stress completion.

## Compatibility Impact

No MyLite SQL behavior changes. The slice strengthens external MariaDB
comparison evidence for DDL/file-lifecycle shapes that are already product
goals for ownerless concurrency.

## Directory And Lifecycle Impact

No MyLite directory-layout change. The generated SQL exercises native InnoDB
file movement across schema directories and native schema-drop file removal
through a MariaDB-compatible client.

## Native Storage Impact

No storage format change. The trace records InnoDB dictionary `SPACE` identity
for a cross-schema rename and proves dropped-schema tables no longer appear in
`information_schema.INNODB_SYS_TABLES` after schema recreation.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. This is shell tooling and documentation only.

## Test Plan

- `bash -n tools/ownerless-ddl-lifecycle-trace`
- `tools/ownerless-ddl-lifecycle-trace --rounds 3 --output DIR --check`
- `tools/ownerless-sql-trace-runner --trace-dir DIR --check`
- `tools/ownerless-sql-trace-suite --output DIR --trace ddl-lifecycle --scale 2 --check`
- Focused Docker-backed MariaDB replay:
  `tools/ownerless-external-mariadb-trace-smoke --output DIR --trace ddl-lifecycle --scale 1`
- Focused CTest for the lifecycle trace and trace suite.
- `git diff --check` and format-check.

## Acceptance Criteria

- The lifecycle trace emits cross-schema `RENAME TABLE` SQL.
- The lifecycle trace emits schema-drop SQL over InnoDB tables.
- The manifest records cross-schema expected sums.
- Check-mode validation proves the new SQL shapes and oracles are present.
- External MariaDB replay reaches `suite_run=ok` and
  `external_mariadb_trace_smoke=ok`.
- Docs keep full external MariaDB/RQG stress marked as remaining planned work.

## Evidence

Focused dependency-free validation passed:

```text
tools/ownerless-ddl-lifecycle-trace --rounds 3 --output /tmp/mylite-ownerless-ddl-lifecycle-cross-schema-check --check
tools/ownerless-sql-trace-runner --trace-dir /tmp/mylite-ownerless-ddl-lifecycle-cross-schema-check --check
tools/ownerless-sql-trace-suite --output /tmp/mylite-ownerless-ddl-lifecycle-cross-schema-suite-check --trace ddl-lifecycle --scale 2 --check
```

Focused Docker-backed MariaDB 11.8 replay passed at scale 1:

```text
trace=ddl-lifecycle
scale=1
trace_count=1
suite_run=ok
external_mariadb_trace_smoke=ok
```

The final oracle reported:

```text
observed_xschema_oracle_rows=4
observed_cross_schema_space_matches=4
observed_drop_schema_space_pairs=4
observed_final_xschema_space=48
expected_final_xschema_space=48
ownerless_ddl_lifecycle_cross_schema_rename_check=ok
ownerless_ddl_lifecycle_drop_schema_check=ok
```

## Risks And Open Questions

- This remains deterministic trace evidence. Long-running randomized
  MariaDB/RQG-style DDL/file-lifecycle stress remains planned.
- The trace validates external MariaDB behavior for these SQL shapes; product
  retained-WAL recovery remains covered by the embedded ownerless selectors.
