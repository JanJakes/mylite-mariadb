# Catalog Free-List Reuse

## Problem

Growable catalog publication currently writes each new catalog image to a fresh
contiguous page chain and advances the file page count. Superseded catalog
chains remain unreachable append-only history. That is safe, but it makes
metadata-heavy workloads grow the primary `.mylite` file even when later catalog
images could reuse earlier catalog-chain space.

This slice adds a narrow durable free-list for superseded catalog-chain runs and
reuses a suitable run for later catalog publication. It is not full compaction,
online vacuum, row-page reuse, index-page reuse, or file shrinking.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite storage owns this behavior; no MariaDB upstream storage-engine source
  path is changed for this slice.
- `packages/mylite-storage/src/storage_format.h` already reserves
  `MYLITE_STORAGE_FORMAT_HEADER_FREE_LIST_ROOT_PAGE_OFFSET`.
- `packages/mylite-storage/src/storage.c` publishes catalog images through
  `publish_catalog_image()`, which computes the new catalog chain size, writes
  contiguous catalog pages, publishes page `0`, and finishes the write journal.
- The recovery journal currently stores protected page ids in its header and
  restores those pages before truncating to the saved header page count.
  Catalog publication protects page `0` and the old catalog root. Free-list
  reuse must also protect the free-list root before mutating it.
- Catalog pages currently validate a contiguous `next_page == page_id + 1`
  chain. The free-list allocator therefore needs to provide a contiguous page
  run for catalog reuse rather than arbitrary single pages.

## Design

- Add a first-party free-list page type with its own magic, type, version,
  checksum, page id, next free-list root page, run start, and run page count.
- Store one free run per free-list node. The node page is the run start page.
  Allocating from a run takes pages from the run tail, so the node page remains
  durable until the whole run is consumed.
- Reclaim superseded catalog-chain pages after successful catalog publication
  and journal removal. Reclamation writes a free-list node over the old catalog
  root and then publishes a header update protected by a second header-only
  journal. A crash before the second header publish loses the free run but does
  not corrupt the catalog.
- Reuse a free run only for non-active catalog publication when the root
  free-list node has enough contiguous pages for the new catalog chain. The
  existing write journal protects page `0`, the old catalog root, and the
  free-list root before the root node is changed.
- Keep active statement and transaction checkpoints append-only for now. Their
  rollback and snapshot rules are already complex, and this slice should not
  mix free-list mutation into nested checkpoint ownership.
- Clear catalog-keyed read caches after a durable catalog publish before old
  page-count identities can become current again through reuse. Durable row
  payload cache entries also carry an in-memory checksum and are discarded
  before use if their copied row bytes no longer match.

## Affected Subsystems

- First-party storage file format: adds a free-list page type.
- Recovery journal: allows write journals to protect the free-list root in
  addition to page `0` and the current catalog root.
- Catalog publication: optionally reuses a durable free run and reclaims the
  previous catalog chain after publish.
- Storage scans: treat free-list pages as known non-row/non-index pages.
- Runtime caches: invalidate catalog-keyed exact-index, live-row, row-id, row
  payload, and index-leaf caches after DDL catalog publication, and validate
  row-payload cache entries before trusting cached bytes.

## Compatibility Impact

No SQL or public C API semantics change. The visible effect is reduced primary
file page growth for metadata-heavy workloads that repeatedly publish catalog
images outside active MyLite statement checkpoints.

## DDL Metadata Routing Impact

Routed DDL still publishes catalog metadata through the same catalog-image
path. The catalog root page may now move to a previously freed run instead of
always moving to the previous file tail.

## Single-File And Lifecycle Impact

The free-list is durable inside the primary `.mylite` file. No new companion
file is introduced. Recovery either restores the old header/free-list root from
the journal or completes with a valid free-list node and header.

## Public API And File-Format Impact

The public storage API is unchanged. Existing format-version-1 files remain
readable because `free_list_root_page == 0` keeps the old behavior. New files
may contain free-list pages under the existing format version.

## Storage-Engine Routing Impact

None. Engine routing remains above the storage page allocator.

## Binary-Size Impact

Small first-party storage code increase only. No dependency is added.

## Test And Verification Plan

- Unit test that a superseded multi-page catalog chain is reclaimed and then
  reused by a later catalog publication without increasing `page_count`.
- Unit test that corrupting a reachable free-list page returns corruption when
  a later catalog publication tries to allocate from it.
- Unit test that a recovery journal protecting a free-list root restores the
  previous header and free-list node after simulated crash recovery.
- Embedded storage-engine smoke coverage for DDL after cached row reads, which
  guards against stale row-payload caches after catalog free-list reuse.
- Run `mylite_storage_test` under storage-smoke dev and ASAN builds.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- Superseded non-active catalog chains are added to a durable free-list after
  successful catalog publication.
- Later non-active catalog publication can reuse a suitable free run.
- Free-list pages are skipped by existing row/index/autoincrement scans.
- Journal recovery can restore a protected free-list root.
- Docs and roadmap distinguish this slice from full compaction and file
  shrinking.

## Risks And Unresolved Questions

- Full row/index page reuse needs a broader allocator and recovery story.
- Active statement and transaction free-list reuse remains intentionally
  append-only until checkpoint ownership rules are extended.
- File shrinking and online vacuum remain separate compaction work.
