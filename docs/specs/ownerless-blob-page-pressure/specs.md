# Ownerless BLOB Page Pressure

## Problem

Ownerless active-reader pressure coverage proves repeated same-row writes and
expanding large-row writes while a repeatable-read snapshot pin remains live.
The page-log reclamation design still treats InnoDB BLOB and compressed-BLOB
page records as snapshot-sensitive data pages that require boundary evidence
before active-pin reclamation can compact them.

MyLite needs focused SQL evidence that native off-page BLOB data follows the
same conservative active-reader pressure lifecycle: an older snapshot keeps its
original long values, peer writers can update BLOB pages and commit, retained
WAL stays present while the pin is live, WAL is reclaimed after the reader
releases, and final long values survive ownerless/native reopen before and
after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_BLOB`, `FIL_PAGE_TYPE_ZBLOB`, and
  `FIL_PAGE_TYPE_ZBLOB2` as native InnoDB page types for BLOB storage.
- `mariadb/storage/innobase/btr/btr0cur.cc` stores externally stored long
  values on BLOB or compressed-BLOB pages and writes the corresponding
  `FIL_PAGE_TYPE` before linking the page through the BLOB pointer.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_requires_oldest_snapshot_boundary()` treats every page type not
  explicitly classified as native support state as requiring oldest-snapshot
  boundary proof. BLOB page types therefore stay snapshot-sensitive.
- `docs/specs/ownerless-active-pin-reclaim/specs.md` records the same design:
  B-tree, R-tree, BLOB, compressed-BLOB, instant-root, legacy-unknown, and
  unrecognized pages need boundary evidence, while undo/allocation/system
  support pages can be checkpointed as latest native recovery state.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector, `blob-page-pressure`.
- Create a `ROW_FORMAT=DYNAMIC` InnoDB table with `LONGBLOB` values large
  enough to create native off-page BLOB pages.
- Verify off-page BLOB pages exist by scanning the closed `.ibd` file for
  native BLOB page types.
- Hold an ownerless repeatable-read snapshot over the original BLOB values.
- Update each row's BLOB payload through separate ownerless writer opens while
  the snapshot pin remains live.
- Verify retained page-version WAL remains present while the pin is live and is
  checkpointed after the reader releases.
- Verify final row totals, BLOB lengths, and BLOB first-byte checksums through
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- A full BLOB size matrix, encrypted/compressed tablespaces, BLOB prefix-index
  DDL, crash injection during BLOB-page writes, background checkpoint
  scheduling, and external MariaDB/RQG pressure oracles. Compressed row-format
  BLOB page pressure is covered separately by
  `ownerless-compressed-blob-page-pressure`.

## Design

The selector uses only existing ownerless runtime behavior:

1. Create `app.ownerless_blob_pressure` with `id`, `value`, and
   `payload LONGBLOB NOT NULL`, `ENGINE=InnoDB ROW_FORMAT=DYNAMIC`.
2. Insert a bounded number of rows with 24 KiB payloads, close the database,
   and scan `datadir/app/ownerless_blob_pressure.ibd` for native BLOB page
   types to prove the test is exercising off-page BLOB storage.
3. Fork a reader that starts `START TRANSACTION WITH CONSISTENT SNAPSHOT` and
   verifies the original row count, value sum, BLOB byte length, and first-byte
   aggregate.
4. In the parent, update each row through separate ownerless writer opens,
   changing both `value` and the BLOB payload. After every commit, verify the
   writer-visible aggregate and retained WAL presence while the reader pin is
   live.
5. Release the reader and verify close-time reclamation checkpoints the WAL.
6. Reopen through ownerless and ordinary native read/write modes, before and
   after forced `.shm` rebuild, and verify final aggregates plus the presence
   of native BLOB pages.

The selector should require no production code changes if existing active-pin
boundary synthesis and reclamation rules already cover BLOB pages correctly.

## Compatibility Impact

SQL behavior is unchanged. The slice adds internal storage-lifecycle evidence
for native BLOB pages under existing repeatable-read isolation: readers keep
their original snapshot, peer commits advance writer-visible state, and retained
WAL is reclaimed after the active pin releases.

## Directory And Lifecycle Impact

No new durable files or layout changes. The test reads the existing native
InnoDB `.ibd` file after closing handles, and exercises existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` lifecycle.

## Native Storage Impact

Native storage format is unchanged. The test deliberately forces native
off-page BLOB storage and checks BLOB page types as evidence that the ownerless
pressure path is covering the intended page class.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `blob-page-pressure` selector.
- Run adjacent pressure selectors: `active-reader-pressure`,
  `expanding-page-pressure`, `active-reader-pressure-limit`, and
  `active-reader-pressure-diagnostics`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Add `blob-page-pressure` to the opt-in ownerless stress preset with a larger
  bounded row count.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks. If the known broad InnoDB log-header checksum abort appears in an
  unrelated broad ownerless run, clean `/tmp` and isolate-rerun the affected
  selector.

## Acceptance Criteria

- The selector proves the table has native BLOB page types in its `.ibd` file.
- A live repeatable-read snapshot keeps seeing the original BLOB aggregates.
- Peer ownerless writers update BLOB payloads and observe advancing aggregates.
- Page-version WAL remains retained while the snapshot pin is live and is
  checkpointed after the reader releases.
- Final BLOB aggregates and native BLOB page presence survive ownerless/native
  reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is focused off-page BLOB pressure coverage, not a full long-value size,
  row-format, encryption, crash-recovery, or external-oracle matrix.
  Compressed row-format BLOB page pressure is covered separately by
  `ownerless-compressed-blob-page-pressure`.
- Independent timer-driven checkpoint scheduling and full external
  MariaDB/RQG pressure stress remain separate ownerless concurrency gaps.
