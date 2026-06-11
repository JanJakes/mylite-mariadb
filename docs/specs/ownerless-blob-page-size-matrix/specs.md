# Ownerless BLOB Page Size Matrix

## Problem

Ownerless BLOB page pressure coverage proves the active-reader lifecycle for one
`ROW_FORMAT=DYNAMIC` `LONGBLOB` payload size. The compatibility matrix still
tracks broader long-value size coverage as planned, so ownerless mode lacks
focused evidence that the same snapshot pin, page-version WAL retention, native
checkpoint, and reopen behavior holds across a small range of off-page long
value sizes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_BLOB`, `FIL_PAGE_TYPE_ZBLOB`, and `FIL_PAGE_TYPE_ZBLOB2` as
  native InnoDB long-value page types.
- `mariadb/storage/innobase/btr/btr0cur.cc` stores externally stored long
  values on BLOB or compressed-BLOB pages and writes the page type before the
  value is linked through the clustered record's BLOB pointer.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_requires_oldest_snapshot_boundary()` treats BLOB page types as
  snapshot-sensitive data pages that require oldest-snapshot boundary proof
  rather than support-page-only checkpoint elision.
- `docs/specs/ownerless-blob-page-pressure/specs.md` records the first
  single-size `ROW_FORMAT=DYNAMIC` BLOB pressure selector and leaves a full
  long-value size matrix as follow-up.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector, `blob-page-size-matrix`.
- Create one `ROW_FORMAT=DYNAMIC` InnoDB table with five `LONGBLOB` payload
  sizes: 12 KiB, 24 KiB, 48 KiB, 96 KiB, and 192 KiB.
- Verify native BLOB page types exist in the closed `.ibd` file.
- Hold a repeatable-read ownerless snapshot over the original payloads.
- Update every row's payload through separate ownerless writer opens while the
  snapshot pin remains live.
- Verify retained page-version WAL remains present while pinned, checkpoints
  after the reader releases, and final payload aggregates survive
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Compressed `KEY_BLOCK_SIZE` variants.
- Encrypted tablespaces.
- Crash injection during BLOB-page writes.
- External MariaDB/RQG pressure oracles.

## Design

The selector reuses the existing BLOB pressure workflow with a fixed row-size
matrix:

1. Create `app.ownerless_blob_size_matrix` with `id`, `value`, and
   `payload LONGBLOB NOT NULL`, `ENGINE=InnoDB ROW_FORMAT=DYNAMIC`.
2. Insert five rows with 12 KiB, 24 KiB, 48 KiB, 96 KiB, and 192 KiB
   payloads.
3. Close the database and scan
   `datadir/app/ownerless_blob_size_matrix.ibd` for native BLOB page types.
4. Fork a reader that starts `START TRANSACTION WITH CONSISTENT SNAPSHOT` and
   verifies the original row count, value sum, payload byte total, and
   first-byte aggregate.
5. In the parent, update each row through a separate ownerless writer open,
   preserving that row's payload size while changing the first byte and value.
   Each commit must leave page-version WAL retained while the reader pin is
   live.
6. Release the reader and verify WAL checkpointing.
7. Verify final aggregates and BLOB page presence through ownerless/native
   reopen before and after forced `.shm` rebuild.

## Compatibility Impact

SQL behavior is unchanged. The slice broadens storage-lifecycle evidence for
native off-page `LONGBLOB` values under MariaDB repeatable-read semantics:
readers keep their original snapshot, peer commits advance writer-visible state,
and retained WAL is reclaimed after the active pin releases.

## Directory And Lifecycle Impact

No new durable files or layout changes. The test reads the native InnoDB `.ibd`
file after closing handles, and exercises existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` lifecycle.

## Native Storage Impact

Native storage format is unchanged. The selector intentionally uses payload
sizes large enough to produce native BLOB page-type evidence under
`ROW_FORMAT=DYNAMIC`.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `blob-page-size-matrix` selector.
- Run adjacent pressure selectors: `blob-page-pressure`,
  `compressed-blob-page-pressure`, `expanding-page-pressure`, and
  `active-reader-pressure`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- The selector proves the table has native BLOB page types in its `.ibd` file.
- A live repeatable-read snapshot keeps seeing the original mixed-size BLOB
  aggregates.
- Peer ownerless writers update each payload size and observe advancing
  aggregates.
- Page-version WAL remains retained while the snapshot pin is live and is
  checkpointed after the reader releases.
- Final mixed-size BLOB aggregates and native BLOB page presence survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is a bounded size matrix, not exhaustive long-value coverage up to
  maximum BLOB/TEXT limits.
- Compressed `KEY_BLOCK_SIZE`, row-format DDL transition, encryption,
  crash-recovery, and external-oracle matrices remain separate work.
