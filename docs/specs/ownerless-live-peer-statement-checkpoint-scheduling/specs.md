# Ownerless Live-Peer Statement Checkpoint Scheduling

## Problem

Ownerless statement-boundary checkpoint scheduling reclaims the page-version
WAL after write, DDL, and transaction-ending statements once the WAL crosses
the configured threshold, but it currently schedules only when the current
process has no live ownerless peer. Close-time reclamation already has a
nonblocking live-peer statement gate and native-write-idle checks, so an idle
live peer unnecessarily keeps WAL retained until the writer closes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` and `mariadb/sql/sql_prepare.cc` finish direct
  and prepared statement execution before control returns to the embedding
  caller; MyLite schedules statement-boundary work after successful
  `mysql_query()` / `mysql_stmt_execute()` handling in
  `packages/libmylite/src/database.cc`.
- `packages/libmylite/src/database.cc`
  `maybe_reclaim_ownerless_page_log_after_statement()` already filters to
  ownerless read/write handles, autocommit statement boundaries, write/DDL or
  transaction-ending SQL, no active page-version pins, and a thresholded WAL.
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` already supports live
  peers by acquiring `acquire_ownerless_live_reclaim_statement_gate()`, then
  requiring no active ownerless native transaction, lock, page-write,
  dictionary, or redo/recovery state before checkpointing.

## Design

Remove the statement scheduler's redundant no-live-peer precondition. The
scheduler should still require:

- ownerless read/write mode,
- an autocommit statement boundary,
- write/DDL or transaction-ending SQL,
- no active page-version pins,
- WAL size at or above `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES`.

The called reclaim path remains responsible for the live-peer gate and native
write/recovery-idle proof. If any peer has an active statement, transaction,
page-write lock, dictionary DDL, redo reservation, or page-version pin, reclaim
returns without changing the WAL.

## Scope

In scope:

- Idle live ownerless peers with zero active page-version pins.
- Direct SQL and no-result prepared statement-boundary scheduling.
- Existing thresholded WAL scheduling.

Out of scope:

- Independent timer-driven checkpoint scheduling.
- Reclaim while active snapshot pins lack boundary proof.
- SQL-level table-lock fault injection.
- New native recovery or DDL/file-lifecycle metadata.

## Compatibility Impact

The change is internal to ownerless WAL reclamation. SQL results, public API
behavior, and on-disk formats do not change. It improves bounded live-peer WAL
pressure behavior without broadening unsupported DDL or recovery claims.

## Directory And Lifecycle Impact

No durable file is added. The existing `concurrency/mylite-concurrency.wal`,
`.ckpt`, `.shm`, and statement-lock files are reused.

## Native Storage Impact

The scheduler still relies on MariaDB/InnoDB checkpoint evidence through
`mylite_ownerless_innodb_make_checkpoint()` and
`mylite_ownerless_innodb_checkpoint_covers_lsn()`. It does not change native
page flushing or redo generation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run `mylite_ownerless_cross_process_sql_test statement-checkpoint-scheduling`.
- Run `mylite_ownerless_cross_process_sql_test live-reclaim` to keep live
  writer and live snapshot-pin negative coverage.
- Build and run the same focused selectors in `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.
- Confirm no `/tmp/mylite-ownerless-*` directories or ownerless test processes
  remain.

## Acceptance Criteria

- No-peer write and transaction-ending statement-boundary reclaim still
  checkpoints the page-version WAL before close.
- An idle live ownerless peer no longer prevents thresholded statement-boundary
  reclaim.
- Live writer and active snapshot-pin cases still retain the WAL until the
  blocking peer state clears.
- Docs no longer list live-peer statement-boundary scheduling as planned, while
  independent timer-driven checkpoint scheduling remains planned.

## Risks

- Statement scheduling must not reclaim while another process is inside a
  write, DDL, redo, page-write, or recovery-sensitive section. The existing
  live-peer reclaim gate and native-write-idle proof remain the safety boundary.
