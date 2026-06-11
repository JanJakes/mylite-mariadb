# Ownerless Compressed BLOB Page Size Matrix

## Problem

Ownerless compressed BLOB coverage proves one `ROW_FORMAT=COMPRESSED
KEY_BLOCK_SIZE=8` payload size and a separate compressed key-block matrix.
The dynamic BLOB matrix now covers longer multi-page values, but compressed
external BLOB chains need the same bounded payload-size evidence because
MariaDB stores them as native `ZBLOB`/`ZBLOB2` pages with the table's compressed
page size.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` as native compressed BLOB
  page types.
- `mariadb/storage/innobase/btr/btr0cur.cc`
  `btr_store_big_rec_extern_fields()` writes compressed external BLOB pages as
  `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2` when the owning tablespace has a nonzero
  compressed page size.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE` and maps the requested key-block size
  to InnoDB's compressed page size.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_requires_oldest_snapshot_boundary()` treats compressed BLOB page
  records as snapshot-sensitive ownerless page-version records.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector,
  `compressed-blob-page-size-matrix`.
- Create one `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` InnoDB table.
- Cover `LONGBLOB` payload lengths of 12 KiB, 24 KiB, 48 KiB, 96 KiB, and
  192 KiB through prepared binary bindings.
- Preserve retained-WAL, post-release checkpoint, native compressed BLOB page
  evidence, and ownerless/native reopen checks.

Out of scope:

- Exhaustive long-value limits.
- Additional compressed `KEY_BLOCK_SIZE` combinations beyond the existing
  key-block matrix.
- Table encryption and page compression.
- Crash injection during compressed long-value writes.
- External MariaDB/RQG BLOB pressure stress.

## Design

Reuse the compressed BLOB page-pressure lifecycle with a fixed size matrix:

1. Create `app.ownerless_compressed_blob_size_matrix` with `id`, `value`, and
   `payload LONGBLOB NOT NULL`, `ENGINE=InnoDB ROW_FORMAT=COMPRESSED
   KEY_BLOCK_SIZE=8`.
2. Insert five rows through prepared BLOB bindings. Payload sizes are 12 KiB,
   24 KiB, 48 KiB, 96 KiB, and 192 KiB.
3. Close the database and scan the table's `.ibd` file at 8 KiB compressed-page
   boundaries for `FIL_PAGE_TYPE_ZBLOB` or `FIL_PAGE_TYPE_ZBLOB2`.
4. Hold a repeatable-read snapshot reader over the original aggregates.
5. Update each row through separate ownerless writer opens while preserving
   that row's payload length.
6. Verify page-version WAL remains retained during the active pin, checkpoints
   after release, and final aggregates survive ownerless/native reopen before
   and after forced `.shm` rebuild.

## Compatibility Impact

SQL behavior is unchanged. The slice broadens documented ownerless storage
lifecycle evidence for native MariaDB compressed external `LONGBLOB` values.
Compatibility remains partial because maximum long-value limits, broader
compressed DDL/encryption/crash matrices, and randomized external oracle stress
remain planned.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The selector uses the MyLite
database directory, its native InnoDB `.ibd` file, and the existing ownerless
concurrency WAL/checkpoint/shared-memory lifecycle.

## Native Storage Impact

Native storage format is unchanged. The widened payload sizes exercise longer
native compressed off-page BLOB chains while preserving MariaDB compressed
table semantics.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test and documentation changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in production embedded
  presets used by CI timing.
- Run the focused `compressed-blob-page-size-matrix` selector.
- Run adjacent `compressed-blob-page-pressure`,
  `compressed-blob-key-block-matrix`, and `blob-page-size-matrix` selectors.
- Run the relevant ownerless CTest subset and ownerless stress subset.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The focused selector passes with five compressed payload sizes.
- Native `ZBLOB`/`ZBLOB2` page-type evidence is present in the closed `.ibd`
  file.
- A live repeatable-read snapshot continues to read the original mixed-size
  compressed aggregates while peer writers update all five rows.
- WAL remains retained during the active pin and checkpoints after release.
- Final aggregate state survives ownerless/native reopen before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- Larger compressed payloads slightly increase focused selector runtime, but
  the row count remains fixed at five and no external replay is added.
- Full compressed long-value limits, compressed DDL option combinations,
  encryption, crash recovery, and long-running external MariaDB/RQG stress
  remain separate work.
