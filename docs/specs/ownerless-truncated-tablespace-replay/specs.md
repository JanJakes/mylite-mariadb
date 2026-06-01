# Ownerless Truncated Tablespace Replay

## Problem Statement

Ownerless stale-reader rebuild coverage already proves retained
reader-boundary WAL does not resurrect a dropped file-per-table table and does
not move a renamed table back to its old path. `TRUNCATE TABLE` is a separate
file-lifecycle shape: MariaDB keeps the SQL table name but invalidates the old
native table contents and recreates/reuses the file-per-table storage.

MyLite needs focused evidence that retained page-version records from the
pre-truncate table image are not treated as no-live recovery authority after
the only remaining ownerless state is a dead reader pin.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_TRUNCATE` as changing data and
  dispatches it through the truncate statement path.
- `mariadb/sql/sql_truncate.h` identifies truncate statements as
  `SQLCOM_TRUNCATE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` handles truncate as native
  InnoDB DDL that clears the table contents and advances dictionary/tablespace
  state.
- Existing ownerless stale-reader rebuild logic treats retained read-view and
  page-pin state without native writer recovery evidence as reader-boundary
  state, checkpoints retained WAL, and rebuilds volatile `.shm` state without
  applying stale page images.

## Design

Add a focused ownerless SQL selector, `truncated-tablespace-replay`, alongside
the existing dropped and renamed tablespace replay selectors:

1. Create an InnoDB file-per-table table with large rows.
2. Start a peer repeatable-read snapshot pin.
3. Update all rows while the pin is live so page-version WAL contains old-table
   page images retained for the reader.
4. `TRUNCATE TABLE` the table and insert a new final row set through the same
   table name.
5. Close the writer and verify retained WAL remains while the reader pin is
   live.
6. Kill the reader so `.shm` contains stale no-live reader state.
7. Reopen ownerless, ordinary native, forced `.shm` rebuild ownerless, and
   ordinary native again, verifying the table exists with only the post-truncate
   rows and the WAL is checkpointed.

## Scope

In scope:

- Product SQL evidence for stale-reader no-live rebuild after `TRUNCATE TABLE`.
- File lifecycle assertions that the final `.frm` and `.ibd` paths exist.
- Ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Durable DDL file-lifecycle metadata for every create/drop/truncate/import
  shape.
- Partition truncate paths.
- External randomized DDL oracles.
- Background checkpoint scheduling.

## Compatibility Impact

SQL behavior is unchanged. The slice expands the current partial
DDL/file-lifecycle recovery evidence: retained pre-truncate page images do not
replace the native post-truncate table state during no-live stale-reader
rebuild.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes existing `datadir/app`
metadata/data files and the existing `concurrency/mylite-concurrency.wal` and
`.shm` lifecycle.

## Native Storage Impact

No storage format changes. The final native InnoDB table remains the authority
once no live ownerless writer recovery evidence exists. Retained reader WAL is
checkpointed, not replayed, during no-live stale-reader rebuild.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds SQL test coverage and
documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `truncated-tablespace-replay` in `embedded-dev`.
- Run adjacent dropped/renamed/truncated tablespace replay selectors in
  `embedded-dev`.
- Run the same focused and adjacent selectors in `ownerless-test-hooks`.
- Run relevant ownerless CTest coverage, `format-check`, `git diff --check`,
  and cached diff checks before commit.

## Acceptance Criteria

- Retained page-version WAL exists after truncate while the reader pin is live.
- After killing the reader, no-live ownerless reopen succeeds and checkpoints
  retained reader-boundary WAL.
- The final table remains present with only the post-truncate rows.
- The final `.frm` and `.ibd` paths remain present.
- Ownerless/native reopen before and after forced `.shm` rebuild all observe
  the same final post-truncate state.

## Risks And Open Questions

- This is still focused SQL evidence for one truncate shape, not durable
  file-lifecycle metadata for every DDL class.
- MariaDB may change internal truncate implementation details, so the test is
  framed around observable final table contents, file presence, retained WAL,
  and successful no-live rebuild.
