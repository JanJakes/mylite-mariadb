# Ownerless Replacement-Copy File-Op Marker Crash

## Problem Statement

Ownerless file-operation marker coverage now proves durable
checkpoint-needed evidence for representative `RENAME TABLE` `FILE_RENAME`,
`CREATE TABLE ... LIKE` `FILE_CREATE`, CTAS populated `FILE_CREATE`,
`TRUNCATE TABLE`, and `DROP TABLE` boundaries. Replacement-copy DDL remains a
separate class because MariaDB must replace an existing native table with a new
copied table definition or populated CTAS table before MyLite reaches
ownerless dictionary finish.

Existing ownerless hook coverage kills `CREATE OR REPLACE TABLE ... LIKE` and
`CREATE OR REPLACE TABLE ... AS SELECT` writers after replacement-copy
completion but before dictionary finish, then proves no-live recovery keeps the
replacement table. This slice adds the narrower durable-boundary assertion:
the killed writer must leave `concurrency/mylite-concurrency.ckpt` marked as
needing a native file-operation checkpoint while another ownerless peer is
still live and before no-live recovery can drain the marker.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_create_table()` begins the CREATE TABLE path
  around line `13467`. For CTAS, it constructs `select_create` around line
  `13685` and dispatches `handle_select()`. For `CREATE TABLE ... LIKE`, it
  dispatches `mysql_create_like_table()` around line `13714`.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` starts
  around line `4850`, builds CTAS destination columns, and calls
  `mysql_create_table_no_lock()` around line `4964`.
- `mariadb/sql/sql_insert.cc:select_create::send_eof()` starts around line
  `5377` and commits the non-temporary CTAS statement after rows have been
  copied.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` starts around
  line `1607` and routes MyLite ownerless file-operation redo path names.
- `mariadb/storage/innobase/fil/fil0fil.cc:fil_ibd_create()` logs
  `FILE_CREATE` around line `2097`.
- `mariadb/storage/innobase/log/log0recv.cc` parses `FILE_DELETE`,
  `FILE_MODIFY`, `FILE_RENAME`, and `FILE_CREATE` through the same file-op
  recovery switch around line `2803`.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()`
  calls `mark_ownerless_native_file_op_checkpoint_before_dictionary_finish()`
  before the `dictionary-before-finish` hook can stop the process.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  `dictionary-create-or-replace-like-crash` and
  `dictionary-create-or-replace-ctas-crash` recovery coverage plus checkpoint
  marker readers for `mylite-concurrency.ckpt`.

## Scope And Non-Goals

In scope:

- Add hook-only marker selectors and CTests for:
  - `dictionary-create-or-replace-like-file-op-marker-crash`,
  - `dictionary-create-or-replace-ctas-file-op-marker-crash`.
- Reuse the existing replacement-copy crash SQL shapes:
  - `CREATE OR REPLACE TABLE app.ownerless_create_replace_like_crash LIKE ...`,
  - `CREATE OR REPLACE TABLE app.ownerless_create_replace_ctas_crash ENGINE=InnoDB
    AS SELECT ...`.
- Assert the native file-op checkpoint marker is set after killing the writer
  at `dictionary-before-finish`, while another ownerless peer is still live.
- Preserve the existing recovery checks for replacement `.frm`/`.ibd` files,
  old-column and old-index absence, replacement metadata, CTAS copied rows,
  post-recovery writes, ordinary native reopen, and forced `.shm` rebuild.

Out of scope:

- Changing MariaDB redo, InnoDB tablespace, MyLite WAL, or checkpoint formats.
- Claiming full replacement/rebuild DDL recovery for every file-lifecycle
  shape.
- `FILE_MODIFY` marker coverage, partition/tablespace import-export classes,
  SQL-level table-lock fault injection, or external MariaDB/RQG stress.

## Design

The implementation refactors the existing replacement-copy crash tests into
shared runners with a boolean marker assertion. The existing selectors keep
calling those runners without the marker assertion. The new marker selectors
call the same runners with the assertion enabled.

The shared live-peer crash helper gains an optional marker-check argument. When
enabled, it asserts
`read_concurrency_native_file_op_checkpoint_needed(database_path)` immediately
after the writer is killed and before the held ownerless peer is released.
That proves durable prefinish file-operation evidence exists before no-live
recovery has a chance to checkpoint and clear it.

## Compatibility Impact

No SQL result, C API, PHP API, native storage format, production behavior, or
directory layout changes. The slice strengthens unsafe-hook crash evidence for
ownerless replacement-copy DDL. Broader DDL/file-lifecycle recovery remains
partial.

## DDL Metadata Routing Impact

The dictionary-generation protocol is unchanged. The killed writer still
leaves recovery-sensitive replacement dictionary state that blocks cleanup
while a live peer exists; no-live reopen remains responsible for rebuilding and
draining it.

## Directory And Lifecycle Impact

The tests exercise existing files inside the MyLite-owned directory:

- `datadir/app/ownerless_create_replace_like_crash.frm`,
- `datadir/app/ownerless_create_replace_like_crash.ibd`,
- `datadir/app/ownerless_create_replace_ctas_crash.frm`,
- `datadir/app/ownerless_create_replace_ctas_crash.ibd`,
- `concurrency/mylite-concurrency.ckpt`,
- `concurrency/mylite-concurrency.shm`.

No new directory entries are introduced.

## Native Storage Impact

No native format changes. The test proves MyLite records durable recovery
evidence for representative MariaDB-native replacement-copy table operations
before ownerless dictionary finish.

## Build, Size, License, And Dependencies

No dependency, license, public API, or production binary-size impact. The new
CTests are compiled and registered only when unsafe ownerless test hooks are
enabled.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the new selectors:
  `dictionary-create-or-replace-like-file-op-marker-crash` and
  `dictionary-create-or-replace-ctas-file-op-marker-crash`.
- Run adjacent selectors:
  `dictionary-create-or-replace-like-crash`,
  `dictionary-create-or-replace-ctas-crash`,
  `dictionary-create-like-file-op-marker-crash`,
  `dictionary-ctas-file-op-marker-crash`, and
  `native-file-op-marker-drain`.
- Run focused hook CTest registration for the file-op marker selectors.
- Build the production `php-embedded-prod` ownerless SQL target and run
  production replacement-copy replay selectors to prove unsafe hooks compile
  out.
- Run focused ownerless DDL stress, `tools/check-ci-production-builds`,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Writers killed at `dictionary-before-finish` after replacement-copy LIKE and
  replacement-copy CTAS leave
  `read_concurrency_native_file_op_checkpoint_needed(database_path)` true
  before the live peer is released.
- Live-peer cleanup remains busy while recovery-sensitive dictionary state is
  present.
- No-live ownerless recovery keeps the replacement table definitions and native
  files.
- The LIKE replacement preserves copied metadata and removes old-column and
  old-index state.
- The CTAS replacement preserves copied rows and removes old-column and
  old-index state.
- Forced `.shm` rebuild and ordinary native reopen keep the replacement table
  state visible.
- The older replacement-copy crash selectors remain available.

## Risks And Follow-Up

- This covers focused representative replacement-copy crash boundaries, not
  every rebuild/replacement or multi-file DDL combination.
- `FILE_MODIFY` marker coverage, broader native redo/checkpoint
  reconciliation, durable DDL lifecycle metadata, and external MariaDB/RQG
  stress remain planned.
