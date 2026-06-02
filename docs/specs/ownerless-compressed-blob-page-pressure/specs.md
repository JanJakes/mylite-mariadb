# Ownerless Compressed BLOB Page Pressure

## Problem

The ownerless BLOB page-pressure slice covers `ROW_FORMAT=DYNAMIC` off-page
`LONGBLOB` pages, but MariaDB stores compressed-table external BLOB data with
separate native page types. MyLite's active-pin reclamation rules treat those
compressed BLOB pages as snapshot-sensitive data pages, so they need focused
ownerless SQL evidence rather than being implied by ordinary dynamic BLOB
coverage.

MyLite needs a bounded selector proving that compressed off-page BLOB pages
follow the same lifecycle: a live repeatable-read snapshot keeps the original
long values, peer ownerless writers can update compressed BLOB payloads, WAL is
retained while the snapshot pin remains live, WAL checkpoints after release,
and final data survives ownerless/native reopen before and after forced
shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_ZBLOB` and `FIL_PAGE_TYPE_ZBLOB2` as the first and subsequent
  BLOB pages for `ROW_FORMAT=COMPRESSED` tables.
- `mariadb/storage/innobase/btr/btr0cur.cc` writes compressed external BLOB
  pages with `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2` when the table has a nonzero
  compressed page size.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates
  `ROW_FORMAT=COMPRESSED` and `KEY_BLOCK_SIZE`, requiring file-per-table
  support and rejecting unsupported temporary/read-only-compressed cases.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_requires_oldest_snapshot_boundary()` treats page types outside the
  explicit native support-page allowlist as requiring oldest-snapshot boundary
  proof. Compressed BLOB page types therefore remain snapshot-sensitive.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector, `compressed-blob-page-pressure`.
- Create a `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` InnoDB table with
  `LONGBLOB` values large enough to create compressed external BLOB pages.
- Use prepared BLOB binding with deterministic low-compressibility payloads so
  the test does not rely on compressible SQL literals.
- Verify `FIL_PAGE_TYPE_ZBLOB` or `FIL_PAGE_TYPE_ZBLOB2` pages exist by
  scanning the closed `.ibd` file at the compressed page size.
- Hold an ownerless repeatable-read snapshot over the original BLOB values.
- Update each row's BLOB payload through separate ownerless writer opens while
  the snapshot pin remains live.
- Verify retained page-version WAL remains present while the pin is live and is
  checkpointed after the reader releases.
- Verify final aggregates and compressed BLOB page presence through
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Compressed row-format DDL transition matrices, every `KEY_BLOCK_SIZE`, table
  encryption, page compression, crash injection during compressed BLOB writes,
  background checkpoint scheduling, and external MariaDB/RQG pressure oracles.

## Design

The selector reuses the active-reader pressure lifecycle from the dynamic BLOB
slice with compressed-table-specific evidence:

1. Create `app.ownerless_compressed_blob_pressure` with `id`, `value`, and
   `payload LONGBLOB NOT NULL`, `ENGINE=InnoDB ROW_FORMAT=COMPRESSED
   KEY_BLOCK_SIZE=8`.
2. Insert a bounded number of rows through prepared statements. The payload
   helper generates deterministic pseudo-random bytes and sets a known first
   byte for aggregate checks.
3. Close the database and scan
   `datadir/app/ownerless_compressed_blob_pressure.ibd` at 8 KiB page
   boundaries for `FIL_PAGE_TYPE_ZBLOB`/`ZBLOB2`.
4. Fork a reader that starts `START TRANSACTION WITH CONSISTENT SNAPSHOT` and
   verifies the original row count, value sum, BLOB byte length, and first-byte
   aggregate.
5. In the parent, update each row through separate ownerless writer opens with
   a new prepared BLOB payload. After every commit, verify the writer-visible
   aggregate and retained WAL presence while the reader pin is live.
6. Release the reader and verify close-time reclamation checkpoints the WAL.
7. Reopen through ownerless and ordinary native read/write modes, before and
   after forced `.shm` rebuild, and verify final aggregates plus native
   compressed BLOB page presence.

No production code change is expected if existing active-pin boundary
synthesis and reclamation rules already handle compressed BLOB page types
correctly.

## Compatibility Impact

SQL behavior is unchanged. The slice expands native storage-lifecycle evidence
to `ROW_FORMAT=COMPRESSED` external BLOB pages under existing repeatable-read
isolation and ownerless page-version WAL retention rules.

## Directory And Lifecycle Impact

No new durable files or layout changes. The test reads the existing native
InnoDB `.ibd` file after closing handles, and exercises existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` lifecycle.

## Native Storage Impact

Native storage format is unchanged. The test deliberately forces compressed
external BLOB storage and checks compressed BLOB page types as evidence that
the ownerless pressure path covers the intended native page class.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `compressed-blob-page-pressure` selector.
- Run adjacent pressure selectors: `blob-page-pressure`,
  `active-reader-pressure`, `expanding-page-pressure`,
  `active-reader-pressure-limit`, and `active-reader-pressure-diagnostics`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Add `compressed-blob-page-pressure` to the opt-in ownerless stress preset
  with a larger bounded row count.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks. Known broad ownerless InnoDB log-header startup aborts should be
  cleaned and isolated to the affected selector.

## Acceptance Criteria

- The selector proves the table has native compressed BLOB page types in its
  `.ibd` file.
- A live repeatable-read snapshot keeps seeing the original compressed BLOB
  aggregates.
- Peer ownerless writers update compressed BLOB payloads and observe advancing
  aggregates.
- Page-version WAL remains retained while the snapshot pin is live and is
  checkpointed after the reader releases.
- Final BLOB aggregates and compressed BLOB page presence survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is focused compressed BLOB pressure coverage, not a full compressed
  row-format, `KEY_BLOCK_SIZE`, encryption, crash-recovery, or external-oracle
  matrix.
- Independent timer-driven checkpoint scheduling and full external
  MariaDB/RQG pressure stress remain separate ownerless concurrency gaps.
