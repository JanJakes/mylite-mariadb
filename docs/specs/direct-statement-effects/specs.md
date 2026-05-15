# Direct Statement Effects

## Goal

Cover direct `mylite_exec()` affected-row and generated insert-id behavior for
non-result statements.

The public API already exposes `mylite_changes()` and
`mylite_last_insert_id()`. This slice adds direct-execution coverage that proves
those values update after representative temporary-table `INSERT`, `UPDATE`,
and `DELETE` statements.

## Non-Goals

- Do not change public API behavior.
- Do not claim durable MyLite storage for temporary tables.
- Do not broaden MariaDB baseline comparison coverage; the current raw embedded
  comparison runtime intentionally avoids initialized MariaDB system tables and
  is not the right place for table-DML probes.
- Do not add multi-statement transaction semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h:430-431` declares `mysql_affected_rows()` and
  `mysql_insert_id()` for connection-level statement effects.
- `mariadb/libmysqld/lib_sql.cc:314-315` copies embedded affected rows and
  insert id into the `MYSQL` connection after execution.
- `mariadb/libmysqld/lib_sql.cc:1295-1307` stores OK-packet affected rows and
  insert id in the embedded result data that `libmylite` reads after
  `mysql_query()`.
- `packages/libmylite/src/database.cc:1231-1238` already maps
  result-producing statements to zero changed rows and copies
  `mysql_affected_rows()` / `mysql_insert_id()` after successful direct
  execution.

## Compatibility Impact

This strengthens the public SQL API coverage for MariaDB-compatible statement
effects:

- multi-row direct `INSERT` reports the affected row count and first generated
  autoincrement id;
- direct `UPDATE` and `DELETE` report affected rows;
- a later direct `INSERT` reports the next generated autoincrement id.

Prepared statement effect coverage remains in the prepared-statement test.

## Design

Extend `mylite_embedded_exec_test` with a direct statement-effect case. The
test opens a normal embedded MyLite database, creates a runtime schema, creates
a temporary autoincrement table, then executes one direct statement per effect
assertion.

Temporary tables keep the test focused on direct SQL API behavior. They use
session-scoped table state and do not make a durable `.mylite` storage claim.

## File Lifecycle

The test creates only transient session state under the per-test temporary
directory. The existing cleanup gate still requires the runtime directory to be
empty after `mylite_close()`.

## Embedded Lifecycle And API

The coverage uses the same `mylite_db` handle for the full statement sequence
and reads effects immediately after each successful statement. No additional
handle state or ownership rule changes are needed.

## Build, Size, And Dependencies

No production code, dependency, or build-profile change is required.

## Test Plan

1. Add direct embedded execution coverage for temporary-table `INSERT`,
   `UPDATE`, `DELETE`, and a second `INSERT`.
2. Assert `mylite_changes()` after each non-result statement.
3. Assert `mylite_last_insert_id()` after generated autoincrement inserts.
4. Run the direct SQL compatibility group and the embedded preset.
5. Run format, tidy, diff, and full relevant preset checks before commit.

## Acceptance Criteria

- Direct `mylite_exec()` non-result statements have committed tests for affected
  row counts.
- Direct generated autoincrement inserts have committed tests for last insert
  id.
- API, compatibility, roadmap, and harness docs describe the coverage without
  overstating broad MariaDB baseline comparison.

## Risks And Open Questions

- The test uses temporary MariaDB tables because durable routed storage has its
  own storage-engine coverage. Broader table-DML comparison against raw MariaDB
  should be designed with a bootstrapped comparison runtime or curated MTR
  subset.
