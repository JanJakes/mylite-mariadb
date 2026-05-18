# Routed ODKU Statement Effects

## Goal

Cover representative public `mylite_changes()` and
`mylite_last_insert_id()` behavior for `INSERT ... ON DUPLICATE KEY UPDATE`
(ODKU) statements executed against durable MyLite-routed tables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:1388-1404` sends the client-visible insert id for
  direct `INSERT` statements from the first successful generated id, an
  explicit `LAST_INSERT_ID(expr)`, or the inserted autoincrement row value.
- `mariadb/sql/sql_insert.cc:2214-2351` turns duplicate-key inserts into
  update-branch execution, clears the current generated id for duplicate
  updates, and preserves ordinary update-style insert-id behavior unless
  `LAST_INSERT_ID(expr)` was evaluated.
- `mariadb/sql/sql_insert.cc:4678-4686` applies the same first-successful-id or
  `LAST_INSERT_ID(expr)` result logic to `INSERT ... SELECT`.
- `mariadb/sql/item_func.cc:4544-4555` makes `LAST_INSERT_ID(expr)` set the
  connection's client-visible insert id.
- `mariadb/libmysqld/lib_sql.cc:309-316` copies embedded direct statement
  affected-row and insert-id values into the `MYSQL` handle.
- `mariadb/libmysqld/lib_sql.cc:357-361` copies prepared execution effects from
  the embedded connection into the `MYSQL_STMT`.
- `packages/libmylite/src/database.cc:1618-1624` maps direct effects into
  `mylite_changes()` and `mylite_last_insert_id()`.
- `packages/libmylite/src/database.cc:1953-1959` maps prepared execution
  insert-id state before non-result affected rows are finalized.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Direct ODKU duplicate updates that do not call `LAST_INSERT_ID(id)`.
- Direct multi-row ODKU with successful generated inserted rows around a
  duplicate update.
- Direct `INSERT ... SELECT` ODKU with successful generated selected rows
  around a duplicate update.
- Direct and prepared ODKU duplicate updates that call `LAST_INSERT_ID(id)`.
- Direct and prepared `INSERT ... SELECT` ODKU duplicate updates that call
  `LAST_INSERT_ID(id)`.
- Affected-row counts for the same representative routed ODKU statements.

## Non-Goals

- Exhaustive routed `LAST_INSERT_ID()` / `mysql_insert_id()` matrices.
- Raw MariaDB-vs-MyLite durable-storage comparison; native MariaDB durable
  engines use sidecars and are not the MyLite storage target.
- Binary log, replication, or wire-protocol statement effects.
- Size-profile reduction work.

## Compatibility Impact

This closes the immediate gap between temporary-table SQL API statement-effect
comparison and durable routed ODKU behavior. MyLite now has focused tests for
the important routed-storage cases:

- duplicate updates without `LAST_INSERT_ID(id)` preserve the previous
  connection insert id, matching the embedded API observation;
- multi-row `INSERT ... VALUES` and `INSERT ... SELECT` ODKU report the first
  successful generated inserted id; and
- direct and prepared `LAST_INSERT_ID(id)` duplicate updates expose the target
  row id through `mylite_last_insert_id()` for both `VALUES` and
  `INSERT ... SELECT` ODKU.

The claim remains representative. Broader statement-effect matrices stay
planned for grouped error-path variants, triggers, views, offset/increment,
integer-width boundaries, and other ODKU expression paths.

## Design

No production change is expected. The public MyLite statement-effect APIs
already read MariaDB's direct and prepared embedded statement effects. The
slice adds storage-smoke assertions around existing routed ODKU execution paths
so durable handler routing and public statement effects are proven together.

## File Lifecycle

No file-format change is required. Durable state remains in the primary
`.mylite` file and the surrounding ODKU tests keep the existing no durable
sidecar close/reopen checks.

## Embedded Lifecycle And API

No public API change is required. The behavior is observable through
`mylite_changes()` and `mylite_last_insert_id()` after successful direct and
prepared non-result statements.

## Storage-Engine Routing

Coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in the
storage-smoke profile. Omitted/default, MyISAM, and Aria first-key tables share
the same durable MyLite path but are not repeated here.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Extend storage-engine smoke ODKU coverage with direct affected-row and
  insert-id assertions for generated duplicate updates, multi-row ODKU, and
  explicit high-value ODKU paths.
- Add prepared routed ODKU coverage for `LAST_INSERT_ID(id)`.
- Extend insert-select ODKU coverage with direct affected-row and insert-id
  assertions for generated duplicate updates and direct/prepared
  `LAST_INSERT_ID(id)` duplicate updates.
- Run the focused storage-engine test, routed DDL/DML and storage-engine
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Direct routed ODKU without `LAST_INSERT_ID(id)` preserves the previous public
  insert id.
- Direct multi-row routed ODKU reports the first successful generated inserted
  id and the expected affected-row count.
- Direct and prepared routed ODKU with `LAST_INSERT_ID(id)` report the target
  row id through `mylite_last_insert_id()`.
- Direct routed `INSERT ... SELECT` ODKU reports the first successful generated
  inserted id and the expected affected-row count.
- Direct and prepared routed `INSERT ... SELECT` ODKU with
  `LAST_INSERT_ID(id)` report the target row id through
  `mylite_last_insert_id()`.
- Docs distinguish this representative routed statement-effect coverage from
  broader ODKU statement-effect matrices.

## Risks And Open Questions

- Prepared `mysql_stmt_insert_id()` has a documented embedded-library caveat
  when the connection insert-id state is stale; this slice covers the explicit
  `LAST_INSERT_ID(id)` idiom where MariaDB sets that state deliberately.
- Triggers, views, and grouped error-path variants may have different
  statement-effect ordering and remain planned.
