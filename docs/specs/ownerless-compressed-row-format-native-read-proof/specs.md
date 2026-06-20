# Ownerless Compressed Row Format Native Read Proof

## Problem

Ownerless compressed row-format DDL refresh already keeps an already-open peer
correct across `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=...`
table-copy rebuilds. The subtle correctness boundary is the first parent
statement after the peer DDL: the caller decides whether page-version reads are
allowed before the refresh path notices a dictionary-generation change.

Because ownerless page-version WAL records are keyed by `(space_id, page_no)`
and do not yet include a table-copy rebuild generation, the same post-DDL
statement must not enable page-version overlays for the rebuilt compressed
table.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc` treats explicit row
  format and key-block-size changes as rebuild-driving `ALTER TABLE` options.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates compressed row
  format and `KEY_BLOCK_SIZE` options against InnoDB file-per-table support.
- MyLite's ownerless page-version index currently addresses retained page
  images by `(space_id, page_no)` and visible LSN; it does not encode a
  per-table rebuild generation.
- MyLite's ownerless refresh path already refreshes dictionary and space-header
  state before statement execution, and the database perf counters expose
  `refresh_page_version_reads_enabled` for focused regression coverage.

## Scope And Non-Goals

In scope:

- Revoke same-statement page-version-read eligibility when dictionary refresh
  observes a peer dictionary-generation change that requires conservative
  native reads.
- Prove the already-open parent path for
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` has ownerless refresh activity but
  zero `refresh_page_version_reads_enabled` after the child rebuild.
- Prove the same native-read boundary for the focused compressed key-block
  matrix selector covering `KEY_BLOCK_SIZE=4` and `KEY_BLOCK_SIZE=16`.
- Keep final metadata, row aggregates, prepared BLOB writes, ownerless/native
  reopen, and forced `.shm` rebuild checks unchanged.

Out of scope:

- Adding a per-space or per-table rebuild-generation stamp to page-version WAL
  or the shared page-version index.
- Re-enabling page-version acceleration for rebuilt table images.
- Broad randomized compressed DDL oracle or RQG stress.

## Design

`refresh_ownerless_external_pages_before_statement()` now rechecks the handle's
`ownerless_peer_dictionary_refresh_requires_conservative_write` flag after
`refresh_ownerless_dictionary_before_statement()` returns. If dictionary refresh
observed a peer generation change while the statement entered with
page-version reads allowed, the refresh path clears same-statement
page-version-read eligibility, releases any handle page-version pin, and closes
the current InnoDB read view before taking the native refresh path.

The compressed row-format SQL selectors enable database perf counters exactly
after the child reports that the compressed rebuild finished. The parent then
runs the post-rebuild metadata reads, table reads, and prepared BLOB insert.
The test asserts:

- at least one ownerless statement refresh was recorded, and
- `refresh_page_version_reads_enabled` stayed zero for that post-peer-DDL
  window.

## Compatibility Impact

SQL results are unchanged. The slice narrows page-version acceleration only
when an already-open ownerless handle has just observed a peer dictionary
generation change that requires conservative native reads. That is the
documented safe behavior until page-version invalidation can distinguish table
copy rebuild generations.

## Directory And Lifecycle Impact

No directory layout or durable file changes. The existing native InnoDB
file-per-table data and MyLite ownerless concurrency files remain inside the
database directory.

## Native Storage Impact

Native storage remains MariaDB-managed. The change prevents MyLite from
overlaying retained pre-rebuild page-version records onto rebuilt compressed
native table pages during the same statement that detects the peer DDL
boundary.

## Public API Impact

No public API changes.

## Binary Size Impact

The production change is a small branch in the existing ownerless refresh path
and adds no dependencies.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `php-embedded-prod`.
- Run focused selectors:
  - `compressed-row-format-ddl`
  - `compressed-row-format-key-block-ddl`
- Run the corresponding `sql-case` selector for the base compressed
  row-format test.
- Run ownerless hook compressed row-format crash selectors when the hook preset
  is available.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The already-open parent observes compressed row-format metadata after the
  peer rebuild and can insert prepared BLOB rows through the rebuilt table.
- Post-peer compressed DDL parent statements record ownerless refresh calls.
- Post-peer compressed DDL parent statements record zero
  `refresh_page_version_reads_enabled`.
- Final compressed metadata, row aggregates, native ZBLOB page evidence,
  ownerless/native reopen, and forced `.shm` rebuild checks still pass.

## Risks And Follow-Up

- Conservative native reads after peer DDL may remain broader than strictly
  necessary. A future rebuild-generation-aware page-version key can recover
  acceleration without permitting old table images to cross rebuild
  boundaries.
- The focused selectors cover deterministic compressed row-format rebuild
  shapes. Long-running external MariaDB/RQG compressed DDL stress remains
  planned.
