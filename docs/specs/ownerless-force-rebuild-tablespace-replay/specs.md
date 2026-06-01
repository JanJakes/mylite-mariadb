# Ownerless Force Rebuild Tablespace Replay

## Problem Statement

Ownerless stale-reader rebuild coverage proves retained reader-boundary WAL
does not resurrect dropped, renamed, truncated, or schema-dropped
file-per-table objects. `ALTER TABLE ... FORCE` is another native DDL class:
MariaDB treats it as an explicit table recreate even when the SQL definition is
unchanged.

MyLite needs focused evidence that retained page-version records from before a
copy-style force rebuild are discarded during no-live stale-reader `.shm`
rebuild, rather than being replayed into the rebuilt native InnoDB table.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `mysql_prepare_alter_table()` maps explicit `ALTER_RECREATE` into a forced
    rebuild and sets `HA_CREATE_INFO::recreate_identical_table` when the
    rebuild has no other schema change.
  - `mysql_recreate_table()` builds an `Alter_info` with `ALTER_RECREATE` for
    recreate-style table administration paths.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  - InnoDB includes `ALTER_RECREATE_TABLE` in the rebuild-sensitive alter flag
    set.
  - The alter implementation calls out the shared
    `ALTER TABLE...FORCE or OPTIMIZE TABLE` handling while creating rebuilt
    index dictionary entries.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::create()` passes
    `!create_info->recreate_identical_table` to the InnoDB create path and
    commits the DDL transaction before writing the commit LSN to the log.
- `packages/libmylite/src/database.cc`
  - `prepare_concurrency_shm_layout()` distinguishes dirty no-live recovery
    rebuilds from no-live stale-reader rebuilds.
  - Stale-reader rebuilds call `discard_stale_reader_page_log()` before
    segment initialization when shared memory contains read-view/page-pin state
    without native writer recovery evidence.
  - Dirty no-live rebuilds with native writer recovery evidence still call
    `replay_concurrency_tablespaces()`.

## Design

Add a focused ownerless SQL selector,
`force-rebuild-tablespace-replay`, beside the existing dropped, renamed,
truncated, and schema-drop stale-reader replay selectors:

1. Create an InnoDB file-per-table table with a secondary index and large row
   payloads.
2. Start a peer repeatable-read snapshot pin.
3. Update every row while the pin is live so page-version WAL retains
   reader-boundary records.
4. Run `ALTER TABLE ... FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`, then insert an
   additional row through the rebuilt table.
5. Verify retained page-version WAL remains after the writer closes cleanly.
6. Kill the reader so no live ownerless process remains and `.shm` contains
   stale reader state.
7. Reopen ownerless, ordinary native, forced `.shm` rebuild ownerless, and
   ordinary native again, verifying the rebuilt table, secondary index, native
   files, row totals, and checkpointed WAL.

## Scope

In scope:

- Product SQL evidence for no-live stale-reader rebuild after copy-style
  `ALTER TABLE ... FORCE`.
- Directory assertions for the rebuilt table's `.frm` and `.ibd` files.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Durable DDL file-lifecycle metadata for every rebuild, import, discard,
  partition, or admin-table path.
- SQL-level table-lock wait fault injection; prior SQL shapes did not reach
  the ownerless table-wait callback.
- External MariaDB/RQG DDL oracles.
- Background checkpoint scheduling.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial DDL/file
lifecycle recovery evidence to include a copy-style InnoDB force rebuild with
retained reader-boundary WAL. Full DDL/file-lifecycle recovery remains partial
until durable lifecycle metadata and broader randomized or oracle-backed
coverage exist.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes existing
`datadir/app/*.frm`, `datadir/app/*.ibd`,
`concurrency/mylite-concurrency.wal`, and
`concurrency/mylite-concurrency.shm` lifecycle.

## Native Storage Impact

No storage format changes. The final MariaDB native state after the forced
rebuild remains the authority when no live ownerless writer recovery evidence
exists. Retained reader WAL is checkpointed, not replayed, during no-live
stale-reader rebuild.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `force-rebuild-tablespace-replay` in `embedded-dev`.
- Run adjacent dropped/renamed/truncated/schema-drop tablespace replay
  selectors in `embedded-dev`.
- Build and run the same focused and adjacent selectors in
  `ownerless-test-hooks`.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- Retained page-version WAL exists after `ALTER TABLE ... FORCE` while the
  reader pin is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- The rebuilt table remains present with its secondary index and final rows.
- The `.frm` and `.ibd` paths remain present.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final rebuilt-table state.

## Risks And Open Questions

- This is focused SQL evidence for one copy-style rebuild shape, not durable
  file-lifecycle metadata for every DDL/admin rebuild class.
- MariaDB may choose different physical space reuse across versions, so the
  test is framed around observable final table/index/file state and successful
  no-live stale-reader rebuild rather than a specific tablespace id change.
