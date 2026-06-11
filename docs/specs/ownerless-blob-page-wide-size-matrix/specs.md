# Ownerless BLOB Page Wide Size Matrix

## Problem

The ownerless BLOB page size matrix covered three `ROW_FORMAT=DYNAMIC`
`LONGBLOB` payload sizes. That proved the active-reader lifecycle across a
small off-page range, but left a gap for longer multi-page external values
inside the same bounded local selector.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_BLOB`, `FIL_PAGE_TYPE_ZBLOB`, and `FIL_PAGE_TYPE_ZBLOB2` as
  native InnoDB long-value page types.
- `mariadb/storage/innobase/btr/btr0cur.cc`
  `btr_store_big_rec_extern_fields()` writes externally stored long values and
  marks uncompressed long-value pages as `FIL_PAGE_TYPE_BLOB`.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_requires_oldest_snapshot_boundary()` treats BLOB page records as
  snapshot-sensitive ownerless page-version records.
- `docs/specs/ownerless-blob-page-size-matrix/specs.md` documents the
  existing dynamic-row-format active-reader matrix selector that this slice
  widens.

## Scope And Non-Goals

In scope:

- Extend the existing `blob-page-size-matrix` ownerless SQL selector from
  three rows to five rows.
- Cover `LONGBLOB` payload lengths of 12 KiB, 24 KiB, 48 KiB, 96 KiB, and
  192 KiB under one live repeatable-read snapshot pin.
- Preserve the existing retained-WAL, post-release checkpoint, native BLOB page
  evidence, and ownerless/native reopen checks.

Out of scope:

- Exhaustive long-value limits.
- Compressed row-format variants.
- Table encryption.
- Crash injection during long-value writes.
- External MariaDB/RQG BLOB pressure stress.

## Design

Reuse `test_ownerless_blob_page_size_matrix_reclaims_after_release()` with a
single matrix-row constant and two additional payload-size cases:

1. Insert five `ROW_FORMAT=DYNAMIC` `LONGBLOB` rows whose payload sizes are
   12 KiB, 24 KiB, 48 KiB, 96 KiB, and 192 KiB.
2. Close the database and verify native `FIL_PAGE_TYPE_BLOB` evidence in the
   table's `.ibd` file.
3. Hold a repeatable-read snapshot reader over the original aggregates.
4. Update each row through separate ownerless writer opens while the snapshot
   remains live, preserving each row's payload length.
5. Verify page-version WAL remains retained during the active pin, checkpoints
   after release, and final aggregates survive ownerless/native reopen before
   and after forced `.shm` rebuild.

## Compatibility Impact

SQL behavior is unchanged. The slice broadens the documented ownerless storage
lifecycle evidence for native MariaDB `ROW_FORMAT=DYNAMIC` off-page
`LONGBLOB` values. Compatibility remains partial because maximum long-value
limits, row-format/encryption/crash matrices, and randomized external oracle
stress remain planned.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The selector continues to use
the MyLite database directory, its native InnoDB `.ibd` file, and the existing
ownerless concurrency WAL/checkpoint/shared-memory lifecycle.

## Native Storage Impact

Native storage format is unchanged. The widened payload sizes are intended to
exercise longer native off-page BLOB chains while preserving the same MariaDB
table definition and row-format semantics.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test and documentation changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the production embedded
  preset used by CI timings.
- Run the focused `blob-page-size-matrix` selector.
- Run adjacent `blob-page-pressure` and `compressed-blob-key-block-matrix`
  selectors.
- Run the relevant ownerless CTest subset and ownerless stress subset.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The focused selector passes with five payload sizes.
- Native BLOB page-type evidence is still present in the closed `.ibd` file.
- A live repeatable-read snapshot continues to read the original mixed-size
  aggregates while peer writers update all five rows.
- WAL remains retained during the active pin and checkpoints after release.
- Final aggregate state survives ownerless/native reopen before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- Larger payloads slightly increase focused selector runtime, but the row count
  remains fixed at five and no external replay is added.
- Full BLOB/TEXT limit coverage, compressed DDL combinations, encryption,
  crash recovery, and long-running external MariaDB/RQG stress remain separate
  work.
