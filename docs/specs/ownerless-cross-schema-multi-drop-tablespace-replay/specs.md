# Ownerless Cross-Schema Multi-Drop Tablespace Replay

## Problem Statement

Ownerless stale-reader replay coverage proves single-table drop, same-schema
same-statement multi-table drop, cross-schema rename, truncate, force-rebuild,
multi-rename, create, recreate, and schema-drop file-per-table final states.
MariaDB also allows one `DROP TABLE` statement to name tables from different
schemas. That shape removes native `.frm`/`.ibd` pairs from multiple schema
directories while preserving the schemas themselves.

MyLite needs focused evidence that no-live stale-reader rebuild skips retained
page-version records for each removed cross-schema file-per-table tablespace,
rather than replaying stale pages into either schema or treating an empty
schema as dropped.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_DROP_TABLE` to
  `mysql_rm_table()`.
- `mariadb/sql/sql_table.cc` `mysql_rm_table()` and
  `mysql_rm_table_no_locks()` process every `TABLE_LIST` entry in a
  multi-table `DROP TABLE`, including schema-qualified names, drive
  DDL-log/table-cache cleanup, call `ha_delete_table()`, and remove SQL `.frm`
  metadata for each successfully dropped table.
- `mariadb/sql/handler.cc` `ha_delete_table()` dispatches each table deletion
  to the owning storage engine.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `ha_innobase::delete_table()` routes InnoDB table deletion through the
  dictionary drop path.
- `mariadb/storage/innobase/dict/drop.cc` and
  `mariadb/storage/innobase/fil/fil0fil.cc` remove dropped InnoDB dictionary
  rows and file-per-table `.ibd` tablespaces after the dictionary operation
  commits.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` supports product
  replay with `MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES`.
- `packages/libmylite/src/database.cc` uses that product replay mode during
  no-live ownerless recovery before rebuilding `.shm`.

## Design

Add a focused ownerless SQL selector,
`cross-schema-multi-drop-tablespace-replay`, beside the stale-reader
tablespace replay selectors:

1. Create one InnoDB file-per-table table in `app` and one in a second schema.
2. Verify both schema directories and native `.frm`/`.ibd` table files exist.
3. Start a peer ownerless process with a repeatable-read snapshot pin.
4. Update both tables while the pin is live so retained WAL contains page
   versions for both tablespaces.
5. Execute one cross-schema `DROP TABLE app.table_a, schema_b.table_b`
   statement.
6. Verify both tables and both native file pairs are absent, the second schema
   still exists, and retained page-version WAL remains while the reader pin is
   live.
7. Kill the reader so no live process remains.
8. Verify ownerless reopen, ordinary native exclusive reopen, forced `.shm`
   rebuild ownerless reopen, and native exclusive reopen all preserve both
   dropped-table absences, the surviving empty schema, and checkpointed WAL.

## Scope

In scope:

- Product SQL evidence for no-live replay skipping multiple missing
  file-per-table tablespaces from one cross-schema `DROP TABLE` statement.
- Directory lifecycle assertions for both schema directories and both native
  `.frm`/`.ibd` file pairs.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.
- Compatibility and ownerless-concurrency documentation updates.

Out of scope:

- Durable file-lifecycle metadata for every create, drop, rename, truncate,
  import, discard, partition, or schema edge case.
- Crash injection between individual table drops inside MariaDB's
  multi-table-drop loop.
- SQL-level table-lock wait fault injection; prior explored SQL shapes stopped
  before the ownerless table-wait callback.
- External MariaDB/RQG DDL oracles.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial ownerless
DDL/file-lifecycle recovery evidence from same-schema multi-table drop to a
cross-schema same-statement drop. Full DDL/file-lifecycle recovery remains
partial until durable lifecycle metadata, broader native redo/checkpoint
reconciliation, and external oracle stress exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's native `DROP TABLE` routing through MyLite's
ownerless dictionary generation boundary. It verifies both dropped table names
are absent from `INFORMATION_SCHEMA.TABLES`, while the non-`app` schema remains
present in `INFORMATION_SCHEMA.SCHEMATA`.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes the existing
`datadir/app/ownerless_cross_drop_replay_a.frm`,
`datadir/app/ownerless_cross_drop_replay_a.ibd`,
`datadir/ownerless_cross_drop_schema/`,
`datadir/ownerless_cross_drop_schema/ownerless_cross_drop_replay_b.frm`,
`datadir/ownerless_cross_drop_schema/ownerless_cross_drop_replay_b.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. MariaDB's final native absence for both dropped
tables remains the storage authority when no live writer recovery evidence
exists. Retained page-version records for both removed tablespaces must be
checkpointed or skipped, not replayed into either schema.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The slice adds focused SQL test coverage and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `cross-schema-multi-drop-tablespace-replay` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Run the embedded ownerless SQL CTest shard containing the new full-suite
  case.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- Both initial tables exist and checkpoint cleanly before the stale reader
  starts.
- Retained page-version WAL exists after the cross-schema multi-table drop
  while the reader pin is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- Both dropped tables are absent from `INFORMATION_SCHEMA.TABLES`.
- The second schema remains present and its directory remains present.
- Both dropped tables reject direct reads.
- Both `.frm` and `.ibd` file pairs remain absent.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final absent-table and surviving-schema state.

## Evidence

Focused embedded selector coverage passed:

```text
cross-schema-multi-drop-tablespace-replay
```

Adjacent stale-reader replay selectors also passed in the same embedded build:

```text
dropped-tablespace-replay
multi-drop-tablespace-replay
cross-schema-multi-drop-tablespace-replay
renamed-tablespace-replay
truncated-tablespace-replay
schema-drop-tablespace-replay
force-rebuild-tablespace-replay
multi-rename-tablespace-replay
created-tablespace-replay
recreated-tablespace-replay
```

The embedded ownerless SQL CTest shard containing the new full-suite case
passed:

```text
ctest --preset embedded-dev -R '^libmylite\.ownerless-cross-process-sql\.0$' --output-on-failure
```

## Risks And Open Questions

- This proves a cross-schema same-statement multi-drop final state. It does
  not prove crash recovery between individual table drops in MariaDB's
  internal loop.
- The broader durable file-lifecycle protocol and external oracle stress
  remain open work.
