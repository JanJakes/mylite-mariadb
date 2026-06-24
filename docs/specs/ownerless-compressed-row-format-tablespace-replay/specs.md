# Ownerless Compressed Row-Format Tablespace Replay

## Problem Statement

Ownerless stale-reader tablespace replay already covers dropped, created,
renamed, truncated, recreated, replacement, and generic force-rebuilt
file-per-table InnoDB tablespaces. Compressed row-format rebuilds are a sharper
DDL file-lifecycle case because the rebuilt table changes InnoDB row-format
metadata and produces native compressed external-value pages. Ownerless
recovery should prove that stale retained page-version WAL from the
pre-rebuild table does not overwrite the compressed final table after the stale
reader dies and `.shm` is rebuilt.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5904` through
  `mariadb/sql/sql_yacc.yy:5908` parses table `KEY_BLOCK_SIZE` options into
  `HA_CREATE_USED_KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when an ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11550` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11615` validates the accepted
  compressed key-block values and `ROW_FORMAT=COMPRESSED` constraints.
- `mariadb/storage/innobase/include/fil0fil.h:1318` through
  `mariadb/storage/innobase/include/fil0fil.h:1321` defines native compressed
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` pages.

## Design

Add a focused ownerless SQL case:
`test_ownerless_compressed_row_format_tablespace_replay_keeps_compressed_space`.

The test:

1. Creates `app.ownerless_compressed_replay` as a dynamic InnoDB
   file-per-table table with prepared `LONGBLOB` payload rows.
2. Records the initial native InnoDB `SPACE` and page-0 identity, then closes
   to a clean checkpointed baseline.
3. Starts a repeatable-read ownerless peer that pins the pre-rebuild snapshot.
4. Updates the original rows while the stale reader is live, then runs
   `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`.
5. Inserts a third prepared `LONGBLOB` row after the compressed rebuild and
   verifies compressed metadata, final aggregates, native page-0 identity, and
   `ZBLOB`/`ZBLOB2` evidence before closing.
6. Kills the stale reader so no-live recovery must handle retained
   pre-rebuild WAL and stale page-version pins.
7. Verifies ownerless and ordinary native reopen, before and after forced
   `.shm` rebuild, preserve only the compressed final table state.

## Scope And Non-Goals

In scope:

- One representative compressed row-format table-copy rebuild with
  `KEY_BLOCK_SIZE=8`.
- Retained stale-reader WAL across the rebuild and no-live recovery after the
  stale reader is killed.
- Ownerless/native reopen and forced `.shm` rebuild of the compressed final
  table state.
- Native compressed page evidence in the rebuilt `.ibd` file.

Out of scope:

- Full `KEY_BLOCK_SIZE=1`/`2`/`4`/`8`/`16` stale-reader replay matrix.
- Page compression, encryption, table `TABLESPACE`, partition, external
  directory, and `DISCARD/IMPORT TABLESPACE` variants.
- New production recovery logic unless the focused test exposes a bug.
- External MariaDB/RQG randomized DDL stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice adds recovery evidence for a
MariaDB-valid compressed row-format rebuild while older page-version WAL is
retained by a live snapshot reader.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test keeps all native InnoDB
files inside the MyLite database directory, verifies the `.frm` and `.ibd`
files remain present for the final table, and forces `.shm` rebuild before the
final ordinary native reopen.

## Native Storage Impact

Native InnoDB remains responsible for the compressed rebuild. MyLite recovery
must avoid replaying stale pre-rebuild page images over the compressed final
tablespace and must preserve native compressed external-value pages.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
diff is test and documentation coverage only unless the test exposes a product
bug.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused production selector:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test compressed-row-format-tablespace-replay`.
- Run the focused production case-table route:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_compressed_row_format_tablespace_replay_keeps_compressed_space`.
- Run adjacent stale-reader replay and compressed row-format selectors.
- Build and run the focused selector under `ownerless-test-hooks`.
- Run `format-check-prod`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- The focused selector leaves retained page-version WAL while the stale reader
  is live and the compressed rebuild has completed.
- After the stale reader is killed, ownerless reopen checkpoints the retained
  WAL and preserves the compressed final table.
- Ordinary native reopen and forced `.shm` rebuild show compressed row-format
  metadata, final aggregates, page-0 identity, and native `ZBLOB`/`ZBLOB2`
  evidence.

## Risks And Follow-Up

- This is representative compressed rebuild stale-reader replay coverage, not
  an exhaustive key-block matrix.
- Broader native redo/checkpoint reconciliation, additional DDL
  file-lifecycle crash windows, active-reader pressure crash/oracle breadth,
  and external MariaDB/RQG stress remain open completion work.
