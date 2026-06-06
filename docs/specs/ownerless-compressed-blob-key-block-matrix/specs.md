# Ownerless Compressed BLOB Key-Block Matrix

## Problem

Ownerless compressed BLOB page pressure currently proves the active-reader
lifecycle for one `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` table. The
compatibility matrix still tracks broader compressed `KEY_BLOCK_SIZE` coverage
as planned, so MyLite lacks focused evidence that the same snapshot pin,
page-version WAL retention, native checkpoint, and reopen behavior holds for
additional compressed page sizes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_options_are_invalid()` accepts
  `KEY_BLOCK_SIZE` values `1`, `2`, `4`, `8`, and `16` when the value fits the
  configured InnoDB page-size maximum and file-per-table prerequisites.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_table_def()` converts the requested
  key-block size from KiB into InnoDB's compressed-page shift and only applies
  it to `ROW_FORMAT=COMPRESSED`.
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` as native compressed BLOB
  page types.
- `mariadb/storage/innobase/btr/btr0cur.cc` reads and frees externally stored
  compressed values using the owning table's `space->zip_size()` and expects
  compressed long-value pages to have `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2` page
  types.
- `docs/specs/ownerless-compressed-blob-page-pressure/specs.md` records the
  first single-key-block compressed BLOB pressure selector and leaves a broader
  `KEY_BLOCK_SIZE` matrix as follow-up.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector,
  `compressed-blob-key-block-matrix`.
- Create four `ROW_FORMAT=COMPRESSED` InnoDB tables using
  `KEY_BLOCK_SIZE=1`, `KEY_BLOCK_SIZE=2`, `KEY_BLOCK_SIZE=4`, and
  `KEY_BLOCK_SIZE=8`.
- Insert deterministic low-compressibility `LONGBLOB` values through prepared
  bindings so the selector does not rely on SQL-literal compression behavior.
- Verify compressed native BLOB page types exist in each closed `.ibd` file
  using that table's compressed page size.
- Hold a repeatable-read ownerless snapshot over the original payloads.
- Update every payload through separate ownerless writer opens while the
  snapshot pin remains live.
- Verify retained page-version WAL remains present while pinned, checkpoints
  after the reader releases, and final aggregates survive ownerless/native
  reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive `KEY_BLOCK_SIZE` coverage for 16 KiB.
- Compressed row-format DDL transitions.
- Table encryption and page compression.
- Crash injection during compressed BLOB writes.
- External MariaDB/RQG pressure oracles.

## Design

The selector reuses the existing compressed BLOB page-pressure lifecycle with a
bounded key-block matrix:

1. Create `app.ownerless_compressed_blob_kb1`,
   `app.ownerless_compressed_blob_kb2`,
   `app.ownerless_compressed_blob_kb4`, and
   `app.ownerless_compressed_blob_kb8`, each with `id`, `value`, and
   `payload LONGBLOB NOT NULL`, `ENGINE=InnoDB ROW_FORMAT=COMPRESSED`, and the
   matching `KEY_BLOCK_SIZE`.
2. Insert a small fixed number of rows into each table with prepared BLOB
   bindings and deterministic first-byte markers.
3. Close the database and scan the four `.ibd` files at 1 KiB, 2 KiB, 4 KiB,
   and 8 KiB boundaries for `FIL_PAGE_TYPE_ZBLOB` or `FIL_PAGE_TYPE_ZBLOB2`.
4. Fork a reader that starts `START TRANSACTION WITH CONSISTENT SNAPSHOT` and
   verifies the original row count, value sum, BLOB byte length, and first-byte
   aggregate across all four tables.
5. In the parent, update each row through a separate ownerless writer open,
   preserving payload length and changing the first-byte aggregate. Each commit
   must leave page-version WAL retained while the reader pin is live.
6. Release the reader and verify WAL checkpointing.
7. Verify final aggregates, `Compressed` row-format metadata, and compressed
   BLOB page presence through ownerless/native reopen before and after forced
   `.shm` rebuild.

## Compatibility Impact

SQL behavior is unchanged. The slice broadens native storage-lifecycle
evidence for compressed InnoDB external BLOB pages under MariaDB
repeatable-read semantics. Compatibility remains partial because exhaustive
key-block sizes, compressed DDL transitions, crash injection, and external
oracle stress are still planned.

## Directory And Lifecycle Impact

No new durable files or layout changes. The test reads native InnoDB `.ibd`
files after closing handles, and exercises the existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` lifecycle.

## Native Storage Impact

Native storage format is unchanged. The selector intentionally uses three
compressed page sizes and verifies compressed BLOB page-type evidence for each
table's native `.ibd` file.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `compressed-blob-key-block-matrix` selector.
- Run adjacent pressure selectors: `compressed-blob-page-pressure`,
  `blob-page-size-matrix`, `blob-page-pressure`, and
  `active-reader-pressure`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Each compressed table has native `ZBLOB`/`ZBLOB2` page evidence in its `.ibd`
  file at the matching compressed page size.
- A live repeatable-read snapshot keeps seeing the original compressed BLOB
  aggregates across all covered key-block sizes.
- Peer ownerless writers update each compressed payload and observe advancing
  aggregates.
- Page-version WAL remains retained while the snapshot pin is live and is
  checkpointed after the reader releases.
- Final compressed BLOB aggregates, row-format metadata, and native compressed
  BLOB page presence survive ownerless/native reopen before and after forced
  `.shm` rebuild.

## Risks And Follow-Up

- This is a bounded 1 KiB / 2 KiB / 4 KiB / 8 KiB matrix, not exhaustive
  `KEY_BLOCK_SIZE` coverage; 16 KiB remains separate because it coincides with
  the default InnoDB page size in the current embedded profile.
- Compressed row-format DDL transition, encryption, crash-recovery, and
  external-oracle matrices remain separate work.
