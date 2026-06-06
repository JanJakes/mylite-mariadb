# Ownerless Multi-Drop Tablespace Replay

## Problem Statement

Ownerless stale-reader replay coverage already proves a single dropped
file-per-table tablespace remains absent when retained page-version WAL exists.
MariaDB also supports dropping multiple tables in one `DROP TABLE` statement.
That shape can remove multiple native `.frm`/`.ibd` pairs under one DDL
boundary while an older ownerless reader still pins page-version WAL.

MyLite needs focused evidence that no-live stale-reader rebuild skips retained
page-version records for every removed tablespace in a same-statement multi
drop, rather than failing recovery or resurrecting one of the dropped tables.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_DROP_TABLE` to
  `mysql_rm_table()`.
- `mariadb/sql/sql_table.cc` `mysql_rm_table()` and
  `mysql_rm_table_no_locks()` process the table list for a multi-table
  `DROP TABLE`, remove table definitions from the cache, record DDL-log state,
  invoke `ha_delete_table()`, and remove native `.frm` files for each table
  whose engine deletion succeeds.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `ha_innobase::delete_table()` routes each table through InnoDB dictionary
  drop and file deletion.
- `mariadb/storage/innobase/dict/drop.cc` and
  `mariadb/storage/innobase/fil/fil0fil.cc` remove dropped dictionary rows and
  file-per-table `.ibd` tablespaces after the dictionary operation commits.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` can ignore
  unresolved tablespaces when product replay is called with
  `MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES`.
- `packages/libmylite/src/database.cc` calls product replay with that
  ignore-missing flag before rebuilding `.shm` during no-live ownerless
  recovery.

## Design

Add a focused ownerless SQL selector,
`multi-drop-tablespace-replay`, beside the existing stale-reader tablespace
replay selectors:

1. Create two InnoDB file-per-table tables with large rows and verify their
   native `.frm` and `.ibd` files exist.
2. Start a separate ownerless process with a repeatable-read snapshot pin.
3. Update both tables while the snapshot pin is live so retained WAL contains
   page-version records for both tablespaces.
4. Execute one `DROP TABLE table_a, table_b` statement.
5. Verify both tables and both native file pairs are absent while the retained
   page-version WAL is not checkpointed.
6. Kill the snapshot owner so no live process remains.
7. Verify ownerless reopen, ordinary native exclusive reopen, forced `.shm`
   rebuild ownerless reopen, and native exclusive reopen all preserve both
   dropped-table absences and checkpoint the WAL.

## Scope

In scope:

- Product SQL evidence for no-live replay skipping multiple missing
  file-per-table tablespaces from one `DROP TABLE` statement.
- Directory/file lifecycle assertions for both `.frm`/`.ibd` pairs.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.
- Compatibility and ownerless-concurrency documentation updates.

Out of scope:

- Durable file-lifecycle metadata for every create, drop, rename, truncate,
  import, discard, partition, or schema edge case.
- Crash injection between individual table drops in a multi-table statement.
- SQL-level table-lock wait fault injection; prior explored SQL shapes stopped
  before the ownerless table-wait callback.
- External MariaDB/RQG DDL oracles.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial ownerless
DDL/file-lifecycle recovery evidence from one dropped file-per-table tablespace
to multiple tablespaces dropped by the same SQL statement. Full
DDL/file-lifecycle recovery remains partial until durable lifecycle metadata,
broader native redo/checkpoint reconciliation, and external oracle stress
exist.

## DDL Metadata Routing Impact

The selector uses MariaDB's native `DROP TABLE` routing through MyLite's
ownerless dictionary generation boundary. It verifies both dropped table names
are absent from `INFORMATION_SCHEMA.TABLES` after no-live rebuild and after
forced `.shm` recreation.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes the existing
`datadir/app/ownerless_multi_drop_replay_a.frm`,
`datadir/app/ownerless_multi_drop_replay_a.ibd`,
`datadir/app/ownerless_multi_drop_replay_b.frm`,
`datadir/app/ownerless_multi_drop_replay_b.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. MariaDB's final native absence for both dropped
tables remains the storage authority when no live writer recovery evidence
exists. Retained page-version records for both removed tablespaces must be
checkpointed or skipped, not replayed.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The slice adds focused SQL test coverage and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `multi-drop-tablespace-replay` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Run focused ownerless SQL CTest shards or the embedded ownerless selector
  covering the new full-case entry.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- Both initial tables exist and checkpoint cleanly before the stale reader
  starts.
- Retained page-version WAL exists after the same-statement multi-table drop
  while the reader pin is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- Both dropped tables are absent from `INFORMATION_SCHEMA.TABLES`.
- Both dropped tables reject direct reads.
- Both `.frm` and `.ibd` file pairs remain absent.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final absent-table state.

## Evidence

Focused embedded selector coverage passed:

```text
multi-drop-tablespace-replay
```

Adjacent stale-reader replay selectors also passed in the same embedded build:

```text
dropped-tablespace-replay
multi-drop-tablespace-replay
renamed-tablespace-replay
truncated-tablespace-replay
schema-drop-tablespace-replay
force-rebuild-tablespace-replay
multi-rename-tablespace-replay
created-tablespace-replay
recreated-tablespace-replay
```

The embedded ownerless SQL CTest shard containing the new full-suite case
passed with `case index=47` and `shard pass index=7 count=8`.

## Risks And Open Questions

- This proves a same-statement multi-drop final state. It does not prove crash
  recovery between individual table drops in MariaDB's internal loop.
- The broader durable file-lifecycle protocol and external oracle stress remain
  open work.
