# Ownerless FLUSH TABLES Lock Policy

## Problem Statement

Ownerless read/write mode already covers ordinary `FLUSH TABLES` as a local
dictionary/table-cache refresh path. The same MariaDB grammar also accepts
`FLUSH TABLES ... WITH READ LOCK` and `FLUSH TABLES ... FOR EXPORT`, which enter
server-level global read lock, locked-table, checkpoint, and InnoDB quiesce
paths that are not covered by the current ownerless concurrency proof.

The slice keeps ordinary ownerless `FLUSH TABLES` available and fails closed for
the lock/export variants until MyLite has an explicit ownerless backup/export
and locked-table lifecycle design.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:14919` parses `FLUSH` into `SQLCOM_FLUSH` and
  `mariadb/sql/sql_yacc.yy:14934` marks `FLUSH TABLE[S]` with
  `REFRESH_TABLES`.
- `mariadb/sql/sql_yacc.yy:14962` accepts `WITH READ LOCK` and adds
  `REFRESH_READ_LOCK`; `mariadb/sql/sql_yacc.yy:14967` accepts `FOR EXPORT` and
  adds `REFRESH_FOR_EXPORT`.
- `mariadb/sql/sql_reload.cc:483` documents
  `FLUSH TABLES <table_list> WITH READ LOCK` and
  `FLUSH TABLES <table_list> FOR EXPORT` as taking exclusive locks, expelling
  table-cache entries, reopening tables, entering locked-tables mode, and
  downgrading locks.
- `mariadb/sql/sql_reload.cc:562` implements `flush_tables_with_read_lock()`
  and rejects callers already in locked-table mode.
- `mariadb/sql/sql_parse.cc:5383` dispatches `SQLCOM_FLUSH`; table-specific
  `REFRESH_READ_LOCK` or `REFRESH_FOR_EXPORT` calls
  `flush_tables_with_read_lock()` before returning success.
- `mariadb/sql/sql_reload.cc:253` handles global `FLUSH TABLES WITH READ LOCK`
  by acquiring the global read lock, flushing all tables, and making the global
  read lock block commit.
- `mariadb/storage/innobase/handler/ha_innodb.cc:16305` checks
  `SQLCOM_FLUSH` with a read lock to start InnoDB table quiesce, and
  `ha_innodb.cc:16327` completes that quiesce on unlock or transaction
  interruption.

## Design

Add a first-party MyLite SQL policy helper that applies only when the handle is
using ownerless coordination. It recognizes:

- `FLUSH TABLE[S] ... WITH READ LOCK`,
- `FLUSH TABLE[S] ... WITH READ LOCK AND DISABLE CHECKPOINT`,
- `FLUSH TABLE[S] ... FOR EXPORT`,
- the same statements with `LOCAL` or `NO_WRITE_TO_BINLOG`.

The policy must not reject ordinary `FLUSH TABLES` or
`FLUSH TABLE[S] <table_list>` without the lock/export suffix because existing
ownerless DDL tests use that path to prove local dictionary refresh remains
readable after table-cache eviction.

## Compatibility Impact

MariaDB accepts these statements in a daemon/server context. MyLite ownerless
mode deliberately rejects the lock/export variants because they are backup and
global-read-lock coordination surfaces, not ordinary application DML/DDL.
Exclusive embedded mode remains unchanged.

## Directory And Native Storage Impact

No directory-format or native-file layout change. The policy prevents
uncoordinated quiesce, checkpoint-disable, and locked-table state from being
entered while multiple ownerless processes may be active.

## Public API Impact

No new C API. `mylite_exec()` and `mylite_prepare()` report a MyLite policy
error before MariaDB dispatches the unsupported ownerless SQL.

## Binary Size And Dependencies

The slice adds token checks and tests only. No dependency or embedded profile
change.

## Test Plan

- Add a focused ownerless SQL selector that creates an InnoDB table, verifies
  ordinary `FLUSH TABLES` and table-specific `FLUSH TABLE` still succeed, and
  rejects the lock/export variants.
- Verify the table and index remain usable through ownerless reopen, ordinary
  native reopen, and forced `.shm` rebuild.
- Run the focused selector in normal embedded and ownerless hook presets.
- Run the full embedded and hook ownerless SQL suites, ownerless stress,
  format check, and diff whitespace check.

## Acceptance Criteria

- Ownerless `FLUSH TABLES ... WITH READ LOCK` and
  `FLUSH TABLES ... FOR EXPORT` fail with a MyLite policy error.
- Ordinary ownerless `FLUSH TABLES` remains supported.
- No persistent files are added outside the MyLite database directory.
- Documentation keeps backup/export and locked-table lifecycle coverage marked
  unsupported/planned rather than claiming full ownerless completion.

## Risks And Unresolved Questions

- This does not implement an ownerless backup protocol or SQL-level global read
  lock. Those require a separate design for reader slots, checkpoint pressure,
  commit blocking, copy/export rules, and crash cleanup.
- SQL-level native table-wait fault injection remains planned separately for
  native wait paths; this policy only prevents user SQL from entering the
  `FLUSH TABLES` lock/export variants.
