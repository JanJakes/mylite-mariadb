# Ownerless Prepared Stale Tablespace Cache Retry

## Goal

Extend the ownerless stale InnoDB dictionary-cache recovery path from text
plain reads to prepared plain reads. An already-open ownerless peer should be
able to prepare and step a plain `SELECT` against a peer-created
file-per-table InnoDB table when MariaDB first reports
`ER_NO_SUCH_TABLE_IN_ENGINE` from stale local dictionary/tablespace state.

## Non-Goals

- Do not retry prepared writes, DDL, locking reads, transaction statements, or
  statements inside explicit transactions.
- Do not add a general prepared-statement replay facility for partially
  executed result cursors.
- Do not broaden DDL/file-lifecycle recovery beyond the existing trigger DDL
  stale-cache reproducer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc:5151` maps handler open failures for an engine-owned
  table to `ER_NO_SUCH_TABLE_IN_ENGINE`.
- `mariadb/sql/sql_base.cc:87` and `mariadb/sql/sql_base.cc:162` treat
  `ER_NO_SUCH_TABLE_IN_ENGINE` as a stale-table-cache condition for server
  table-open recovery paths.
- `mariadb/storage/innobase/dict/dict0load.cc:2236` loads a table's
  tablespace through the process-local InnoDB `fil_system` cache.
- `mariadb/storage/innobase/fil/fil0fil.cc:2230` opens and validates a
  file-per-table tablespace before registering it locally.
- `packages/libmylite/src/database.cc` already retries ownerless text plain
  reads once after MariaDB errno `1932` by refreshing native pages, running
  `FLUSH TABLES`, evicting InnoDB dictionary cache entries, clearing MyLite's
  ownerless FK cache, and retrying the query.
- `packages/libmylite/src/database.cc` stores ownerless prepared SQL text and
  policy tokens on `mylite_stmt`, so a failed prepared plain read can be
  reprepared and rebound without widening the retry to mutating SQL.

## Compatibility Impact

No SQL syntax changes. Ownerless prepared `SELECT` compatibility improves for
the same stale peer-created tablespace case already covered by text reads.
The retry is limited to autocommit prepared plain reads because retrying writes
or DDL can re-execute side effects.

## Design

When `mysql_stmt_prepare()` or `mysql_stmt_execute()` reports MariaDB errno
`1932` for an ownerless prepared plain read outside an explicit transaction,
MyLite uses the existing stale-cache refresh helper, prepares a replacement
native `MYSQL_STMT`, verifies the parameter count has not changed for the
step-time path, rebinds the saved parameters, rebuilds result metadata, and
executes once more.

The original native prepared statement is not reused for step-time retry,
because it may hold process-local table or dictionary state from before the
refresh. The retry path stays under the existing ownerless statement boundary,
dictionary-generation checks, page-version read pin, and cleanup logic.

## File Lifecycle

No new files or directory layout are introduced. Durable state remains in the
MyLite database directory. The slice refreshes process-local native caches so a
live peer can open a table whose files already exist in the directory.

## Embedded Lifecycle And API

The public prepared-statement API is unchanged. `mylite_prepare()` may recover
once from the stale engine error for eligible ownerless plain reads.
`mylite_step()` may reprepare the internal native statement once before
returning the first row. Diagnostics still surface the final MariaDB/MyLite
error if the retry fails.

## Build, Size, And Dependencies

No dependencies, build-profile changes, or durable runtime assets are added.
The code uses existing MyLite and MariaDB embedded APIs.

## Test Plan

- Rebuild `mylite_ownerless_cross_process_sql_test` with `embedded-prod` and
  `ownerless-test-hooks`.
- Run the focused trigger DDL selector in both builds:
  `mylite_ownerless_cross_process_sql_test trigger-ddl`.
- Run the direct SQL case by name in both builds:
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_trigger_ddl_refreshes_peer_dictionary`.
- Run the embedded production CTest shard containing the trigger DDL case.
- Run production build guards, CI production-build audit, format check, and
  `git diff --check`.

## Acceptance Criteria

- The trigger DDL ownerless peer test reads the peer-created audit table
  through a prepared `SELECT` before any text read warms the cache.
- The same peer then reads the audit table through text SQL and observes its
  native tablespace registration.
- Prepared retry remains limited to ownerless plain reads outside explicit
  transactions.
- Existing ownerless dictionary and native file-lifecycle focused tests still
  pass.

## Risks And Open Questions

- This is not a full DDL/file-lifecycle proof. Rename, drop, truncate,
  rebuild, and crash-window classes remain covered only by their existing
  focused slices or planned work.
- If MariaDB later reports additional stale dictionary errors for this path,
  they should be added only with focused reproducer evidence.
