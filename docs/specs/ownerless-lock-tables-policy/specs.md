# Ownerless LOCK TABLES Policy

## Problem

Ownerless read/write mode has directory-owned primitives for InnoDB table-lock
wait entries, but MyLite does not yet have a reliable SQL-level `LOCK TABLES`
fault path or a design for connection-level locked-table mode across ownerless
processes. Prior ownerless table-lock waiter-death work explicitly found that
exploratory SQL shapes were intercepted before reaching the intended InnoDB
table-wait callback.

MariaDB `LOCK TABLES` is not a one-statement row-lock operation. It commits the
current transaction, opens and locks tables, enters a connection-level locked
tables mode, keeps handler locks until `UNLOCK TABLES`, and changes which
tables later statements may use. MyLite ownerless mode should fail closed for
that surface until it has a source-backed design for SQL table-lock lifecycle,
MDL interaction, handler external locks, and crash cleanup.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `LOCK TABLE[S] ...` into
  `SQLCOM_LOCK_TABLES` and `UNLOCK TABLE[S]` into `SQLCOM_UNLOCK_TABLES`.
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_LOCK_TABLES` by committing the
  current transaction, unlocking previously locked tables, releasing
  transactional metadata locks, setting `OPTION_TABLE_LOCK`, pre-opening
  temporary tables, checking table-lock privileges, and calling
  `lock_tables_open_and_lock_tables()`.
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_UNLOCK_TABLES` by unlocking the
  `locked_tables_list`, releasing transactional locks, and clearing
  `OPTION_TABLE_LOCK`.
- `mariadb/sql/sql_base.cc:Locked_tables_list::init_locked_tables()` enters
  `LTM_LOCK_TABLES` mode and stores per-connection locked table metadata.
- `mariadb/sql/sql_base.cc:Locked_tables_list::unlock_locked_tables()` leaves
  locked-table mode and closes/unlocks the held tables.
- `mariadb/sql/lock.cc` documents that `LOCK TABLES` calls handler
  `external_lock()` and keeps table locks across later statements until
  `UNLOCK TABLES`.
- `docs/specs/ownerless-table-lock-waiter-death/specs.md` deliberately limits
  current evidence to primitive table wait-entry cleanup and keeps SQL-level
  table-lock fault injection planned.

## Scope And Non-Goals

- Reject ownerless `LOCK TABLES` and `LOCK TABLE`.
- Reject ownerless `UNLOCK TABLES` and `UNLOCK TABLE`.
- Verify rejected statements leave an ordinary InnoDB table and secondary index
  usable through ownerless reopen, native exclusive reopen, and forced `.shm`
  rebuild.
- Do not implement SQL locked-table mode, connection-scoped lock lifecycle,
  handler external-lock coordination, or SQL-level table-lock fault injection.
- Do not change ordinary exclusive read/write MariaDB behavior.

## Design

- Add an ownerless-only SQL policy predicate in
  `packages/libmylite/src/database.cc`.
- Reject statements whose first token is `LOCK` or `UNLOCK` and whose second
  token is `TABLE` or `TABLES`.
- Return a MyLite policy error before MariaDB dispatch, matching the existing
  unsupported ownerless policy shape with MariaDB errno zero.
- Add a focused `lock-tables-policy` ownerless SQL selector that creates an
  ordinary InnoDB table, rejects representative `LOCK TABLES`, `LOCK TABLE`,
  and `UNLOCK TABLES` statements, inserts after rejection, and verifies final
  table/index state through ownerless/native reopen before and after forced
  shared-memory rebuild.

## Compatibility Impact

Ownerless read/write mode explicitly does not support SQL locked-table mode.
This narrows the current compatibility claim: primitive table-lock wait cleanup
exists, but SQL `LOCK TABLES` remains unsupported until MyLite can coordinate
MariaDB's connection-level locked-table lifecycle across independent embedded
processes.

Ordinary non-ownerless embedded behavior is unchanged.

## Directory And Lifecycle Impact

No new files or directory layout changes are introduced. The policy prevents
ownerless mode from entering a SQL state whose locks can outlive the statement
that created them.

## Native Storage Impact

No native storage format changes. Supported ordinary InnoDB tables remain
readable and writable; SQL locked-table mode remains future ownerless work.

## Binary Size And Dependencies

No dependency or license changes. Binary impact is limited to one SQL policy
predicate and focused test code.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `lock-tables-policy` in `embedded-dev`.
- Build and run focused `lock-tables-policy` in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- Ownerless `LOCK TABLES`, `LOCK TABLE`, `UNLOCK TABLES`, and `UNLOCK TABLE`
  fail with a MyLite policy error before MariaDB dispatch.
- Rejected statements leave the base InnoDB table, rows, and secondary index
  usable through ownerless/native reopen before and after forced `.shm`
  rebuild.
- Existing ownerless SQL, hook, and stress coverage remains green.

## Risks And Follow-Up

- Future support needs a design for MariaDB `LTM_LOCK_TABLES`, MDL locks,
  handler external locks, `UNLOCK TABLES`, process death while SQL table locks
  are held, and deterministic SQL-level table-lock fault injection.
