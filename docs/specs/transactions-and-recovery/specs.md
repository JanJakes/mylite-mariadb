# Transactions And Recovery

## Goal

Enable the embedded runtime to use native InnoDB files inside the MyLite
database directory and validate controlled transaction commit, rollback,
savepoints, clean reopen, and process-crash recovery behavior through
`libmylite`.

## Non-Goals

- Do not change the default storage engine in this slice; later default-engine
  work lets MariaDB's compiled default control no-engine table creation.
- Do not claim complete InnoDB compatibility, all isolation levels, foreign-key
  behavior, online DDL, or every recovery mode.
- Do not claim cross-process writer safety or multi-writer behavior; that
  belongs to the locking and concurrency slice.
- Do not implement a custom transaction manager or recovery log.
- Do not add public transaction APIs; this slice uses SQL transaction
  statements through `mylite_exec()`.
- Do not perform size profile hardening.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:3876-3895` maps the embedded
  server's MySQL data directory to InnoDB's default data home.
- `mariadb/storage/innobase/handler/ha_innodb.cc:3923-3939` parses the InnoDB
  system and temporary tablespace paths from the configured data home.
- `mariadb/storage/innobase/handler/ha_innodb.cc:3952-3968` defaults undo and
  redo locations to the embedded data directory when explicit paths are not
  provided.
- `mariadb/storage/innobase/handler/ha_innodb.cc:19113-19115` exposes
  `innodb_log_group_home_dir`, the path for `ib_logfile0`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:19094-19106` documents
  `innodb_flush_log_at_trx_commit` durability tradeoffs; value `1` flushes at
  each commit.
- `mariadb/sql/transaction.cc:254-295` routes `COMMIT` through
  `ha_commit_trans()`.
- `mariadb/sql/transaction.cc:371-392` routes `ROLLBACK` through
  `ha_rollback_trans()`.
- `mariadb/sql/transaction.cc:650-690`, `711-748`, and `766-782` implement
  `SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, and `RELEASE SAVEPOINT` over storage
  engine savepoint hooks.
- `mariadb/storage/innobase/handler/ha_innodb.cc:4531-4555` implements InnoDB
  commit handling.
- `mariadb/storage/innobase/handler/ha_innodb.cc:4620-4704` implements InnoDB
  transaction and statement rollback.
- `mariadb/storage/innobase/handler/ha_innodb.cc:4864-4880` implements InnoDB
  rollback to a savepoint.
- `mariadb/sql/handler.h:4001-4041` distinguishes transactional engines and
  rollback-capable handlers from non-transactional engines.

The embedded archive already includes InnoDB. This slice enables it at runtime
for explicit `ENGINE=InnoDB` tables and pins InnoDB's data, undo, redo, and
temporary locations to the MyLite database directory.

## Compatibility Impact

This slice introduces partial InnoDB coverage for explicit `ENGINE=InnoDB`
tables. It originally left MyLite's temporary MyISAM default unchanged; a later
default-engine slice removes that override so no-engine DDL follows MariaDB's
compiled default. MyISAM remains non-transactional; tests and docs must not
imply MyISAM rollback support.

## Design

For durable database paths, start MariaDB with explicit InnoDB-owned paths under
the MyLite database directory:

- `--innodb-data-home-dir=<db>/datadir`
- `--innodb-log-group-home-dir=<db>/datadir`
- `--innodb-undo-directory=<db>/datadir`
- `--innodb-tmpdir=<db>/tmp`
- `--innodb-temp-data-file-path=ibtmp1:12M:autoextend`
- `--innodb-flush-log-at-trx-commit=1`
- `--innodb-fast-shutdown=1`
- `--innodb-buffer-pool-dump-at-shutdown=OFF`
- `--innodb-buffer-pool-load-at-startup=OFF`

The buffer-pool dump/load feature uses the advisory `ib_buffer_pool` file in
the InnoDB data directory. MyLite disables it because concurrent embedded
processes can race on that single advisory file, while the feature is not
required for durable transaction recovery. Runtime SQL assignments to InnoDB
buffer-pool dump/load variables are also rejected by the
`innodb-buffer-pool-dump-load-policy` slice so applications cannot re-enable
that advisory file path after startup.

At the time of this slice, MyLite kept the temporary MyISAM default to avoid
silently shifting no-engine DDL to InnoDB before explicit default-engine
coverage existed. Current startup no longer forces MyISAM.

Add an embedded integration test that:

1. Creates an explicit InnoDB table.
2. Verifies commit, rollback, savepoint rollback, and release savepoint through
   SQL statements.
3. Closes and reopens the directory to verify committed state persists.
4. Forks a child process that opens the directory, commits one transaction,
   leaves another transaction uncommitted, and exits without `mylite_close()`.
5. Reopens the same directory in the parent, letting InnoDB recover, and asserts
   committed rows survive while uncommitted rows are rolled back.

## File Lifecycle

Expected InnoDB companions must stay under the MyLite database directory,
including system tablespace, temporary tablespace, undo, and redo files. The
test should assert representative InnoDB files under `datadir/` and continue
asserting that the external runtime root stays empty for durable paths.

Stale `run/` state after the child process exits uncleanly is MyLite runtime
state. The existing directory lifecycle policy removes inactive `run/` before
the recovery open starts.

## Embedded Lifecycle And API

The public API does not change. Transaction control is exercised through
MariaDB SQL statements executed by `mylite_exec()`. The crash-recovery test uses
a child process to avoid calling `mylite_close()` and to prove parent recovery
from process death rather than normal cleanup.

Repeated embedded InnoDB open/close cycles require process-global cleanup that
MariaDB's daemon lifecycle normally does not need to repeat. This slice keeps
those fixes narrow: reset InnoDB dictionary pointers, persistent-statistics and
FTS background state, redo group commit locks, shutdown/pre-shutdown state, and
monitor LSN baseline between embedded lifecycles. Charset cleanup also resets
shared UCA 14.0 collation state so restarted MyISAM string indexes do not use
freed UCA weight tables.

## Build, Size, And Dependencies

The embedded archive already includes InnoDB, so no archive-size change is
expected. Runtime startup will create additional InnoDB files under `datadir/`.
The slice should measure the archive as evidence but must not trim or harden the
size profile.

## Test Plan

1. Enable InnoDB runtime paths under the MyLite database directory. At the time
   of this slice, MyISAM remained the temporary default; current startup follows
   MariaDB's compiled default.
2. Add `libmylite.embedded-transactions-recovery`.
3. Cover InnoDB commit, rollback, savepoint rollback, release savepoint, close,
   and reopen.
4. Cover process-crash recovery with committed and uncommitted InnoDB rows.
5. Assert InnoDB files stay inside `datadir/`, `run/` is cleaned on normal
   close, and the external runtime root remains empty.
6. Run embedded and non-embedded build/test presets, format check, tidy, diff
   check, and size measurement.

## Acceptance Criteria

- Existing MyISAM tests still pass with InnoDB enabled but not default.
- Explicit InnoDB tables commit, roll back, and roll back to savepoints through
  `mylite_exec()`.
- Committed InnoDB data survives close/reopen and an unclean child-process exit.
- Uncommitted InnoDB data from the unclean child process is absent after parent
  recovery.
- InnoDB durable companions stay inside the MyLite database directory.
- Docs and compatibility tables state the partial InnoDB transaction/recovery
  surface and remaining limits.

## Risks And Open Questions

- InnoDB restart behavior in repeated embedded open/close cycles may expose
  additional upstream globals that need narrow reset fixes.
- Fork-based crash testing proves process-death recovery, not power-loss
  durability or filesystem-fault recovery.
- InnoDB file names and layout can vary across MariaDB versions; tests should
  assert stable representative files and directory containment rather than every
  internal companion path.
- Locking and cross-process writer safety remain unsupported until the next
  roadmap slice.
