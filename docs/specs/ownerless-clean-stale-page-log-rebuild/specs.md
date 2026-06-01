# Ownerless Stale Reader Page-Log Rebuild

## Problem Statement

Active ownerless snapshot readers can keep page-version WAL records after a
writer has already flushed and checkpointed the native InnoDB pages needed for
latest committed state. Those retained records are reader boundaries, not
no-live crash recovery records.

If all ownerless processes are gone but `mylite-concurrency.shm` still contains
stale reader-only state, rebuilding `.shm` should not replay those retained
reader-boundary records into native tablespace files. Dirty shared memory that
still carries native writer recovery evidence keeps the existing no-live replay
path.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` routes table renames through
    `handler::ha_rename_table()` and renames `.frm` metadata after the engine
    rename succeeds.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` starts an InnoDB DDL transaction, locks
    dictionary/statistics tables, calls `innobase_rename_table()`, commits,
    and writes the DDL transaction log up to the commit LSN.
  - `innobase_rename_table()` calls `row_rename_table_for_mysql()`.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_rename_table_for_mysql()` opens the old dictionary table by name and
    drives dictionary rename logic.
- `mariadb/storage/innobase/dict/dict0dict.cc`
  - `dict_table_rename_in_cache()` calls
    `dict_table_t::rename_tablespace()` for file-per-table tables.
  - `dict_table_t::rename_tablespace()` renames the underlying `.ibd` file
    through the file-space object.
- `packages/libmylite/src/database.cc`
  - `prepare_concurrency_shm_layout()` already distinguishes dirty no-live
    rebuilds from clean `.shm` files whose process slots are all dead.
  - Dirty no-live rebuilds call `replay_concurrency_tablespaces()` before
    rebuilding segments.
  - A killed repeatable-read ownerless reader leaves `.shm` dirty because its
    process slot never releases, even though the remaining read-view/page-pin
    state is not native writer recovery evidence.

## Design

When no ownerless process is live and `.shm` has stale read-view or page-pin
state without InnoDB lock/page-write, dictionary-DDL, or redo recovery evidence,
MyLite will rebuild volatile segments but first checkpoint the page-version WAL
to an empty payload without replaying it to tablespaces.

Dirty no-live shared memory that still carries native writer recovery evidence,
rebuilding shared memory, incompatible shared-memory layout, and invalid native
write-state segments keep the existing conservative recovery behavior.

The focused SQL test will:

1. Create a target schema and an InnoDB file-per-table table.
2. Start an old repeatable-read ownerless snapshot pin.
3. Update the table and rename it across schemas while the pin is live.
4. Verify retained page-version WAL remains after the writer closes cleanly.
5. Kill the snapshot owner, leaving dirty no-live `.shm` with stale reader
   state but no native writer recovery evidence.
6. Reopen ownerless and verify the stale-reader rebuild discards retained
   reader-boundary WAL before SQL execution instead of replaying stale page
   images.
7. Verify the moved table, moved `.frm`/`.ibd`, updated rows, native exclusive
   reopen, and forced `.shm` rebuild all preserve final state.

## Scope

In scope:

- No-live stale-reader `.shm` rebuild policy for retained page-version WAL.
- SQL evidence using cross-schema InnoDB tablespace rename under a live
  snapshot pin.
- Documentation updates for recovery anchors and DDL/file-lifecycle coverage.

Out of scope:

- Dirty no-live replay changes.
- Durable file lifecycle metadata for every DDL operation class.
- Background checkpoint scheduling.
- External MariaDB/RQG DDL oracles.

## Compatibility Impact

SQL behavior is unchanged. The change prevents stale reader-boundary records
from being used as no-live recovery input after all owners are dead and no
native writer recovery evidence remains. Ownerless readers still use those
retained records while they are alive.

## Directory And Lifecycle Impact

No new files or directory layout changes are introduced. The change only
chooses how to handle existing `concurrency/mylite-concurrency.wal` during a
no-live stale-reader `.shm` rebuild.

## Native Storage Impact

Native InnoDB files remain the final authority once no-live state contains only
stale reader evidence. Dirty no-live rebuilds with native writer recovery
evidence still apply visible page-version WAL records to native files before
rebuilding ownerless shared memory.

## Public API Impact

No public API changes.

## Binary Size Impact

No dependency or public surface changes. The implementation adds one small
first-party recovery helper and one SQL selector.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `renamed-tablespace-replay` in `embedded-dev`.
- Run adjacent recovery selectors in `embedded-dev`.
- Build and run focused/adjacent selectors in `ownerless-test-hooks`.
- Run full embedded and hook ownerless SQL CTest coverage.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- No-live stale-reader `.shm` rebuild checkpoints retained reader-boundary WAL
  before SQL execution without replaying stale records.
- Dirty no-live rebuild with native writer recovery evidence still uses product
  tablespace replay.
- The moved InnoDB table remains at the target schema/name with updated rows.
- The source `.frm`/`.ibd` files remain absent and the target files remain
  present through ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Open Questions

- The rule depends on stale reader state being distinguishable from native
  writer recovery evidence through the shared read-view/page-pin, InnoDB lock,
  page-write, dictionary, and redo segments. Dirty writer states remain
  conservative.
- This does not replace durable DDL file-lifecycle metadata; it only prevents
  stale reader-retention records from being treated as crash recovery records.
