# Multi-Page Catalog Chain

## Problem

The MyLite catalog still behaves as a single 4 KiB root page. Table, schema,
foreign-key, and index-root metadata records are appended to that page, and
catalog mutations fail with `MYLITE_STORAGE_FULL` once the root has no
remaining room. That limit now constrains ordinary metadata growth and prevents
larger indexed schemas from publishing catalog-backed index roots.

The storage format already reserves a catalog `next_page` pointer, but the
current validator rejects any nonzero pointer. Simply updating overflow pages
in place would be unsafe because the rollback journal protects only the old
header and old catalog root page. The catalog needs to grow without widening
that journal contract first.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines the discovery callbacks used by engines:
  `discover_table`, `discover_table_names`, and
  `discover_table_existence`. MyLite wires those callbacks in
  `mariadb/storage/mylite/ha_mylite.cc` and answers them from
  `mylite_storage_read_table_definition()`, `mylite_storage_list_tables()`,
  and `mylite_storage_table_exists()`.
- `mariadb/sql/handler.cc::ha_create_table()` suppresses immediate `.frm`
  writes for engines with discovery. MyLite relies on that path so table
  definitions can be stored in the primary `.mylite` catalog instead of durable
  MariaDB sidecars.
- `packages/mylite-storage/src/storage_format.h` reserves
  `MYLITE_STORAGE_FORMAT_CATALOG_NEXT_PAGE_OFFSET`, but only defines the root
  catalog page type and limits rollback journals to header plus catalog root
  through `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`.
- Before this slice,
  `packages/mylite-storage/src/storage.c::validate_catalog_root_bytes()`
  checked that `next_page == 0`, so existing files could only contain a single
  catalog page.
- Before this slice, `packages/mylite-storage/src/storage.c::read_catalog_root()`
  read one page and the catalog find/list/remove/append helpers scanned that
  one in-memory page.
- Catalog write paths, including
  `mylite_storage_store_table_definition()`,
  `mylite_storage_store_schema_definition()`,
  `mylite_storage_store_foreign_key_definition()`,
  `mylite_storage_store_index_root()`,
  `mylite_storage_drop_table()`, and `rename_table()`, mutate the root page and
  then write the root and header under the rollback journal.
- `restore_recovery_journal()` restores the journaled pages and then truncates
  to the saved header page count. That is sufficient for append-only catalog
  publication, but not for in-place edits to non-root catalog pages.

## Design

- Replace catalog callers' mutable one-page buffer with an owned catalog image:
  a contiguous in-memory byte range containing the catalog header followed by
  all catalog records from every page in chain order.
- Read a catalog image by validating the root page and then following
  `next_page` until zero. Each page must:
  - use the existing root catalog page type and format version;
  - match the header catalog generation;
  - have a page id below the header page count;
  - keep records page-local;
  - point only to the next contiguous catalog page or zero.
- Keep the storage format version unchanged. The reserved `next_page` field is
  activated without adding new durable fields.
- Publish every catalog mutation as a new append-only chain at the current file
  tail, after any new blob, row, or index pages written by the same mutation.
  The updated header repoints `catalog_root_page` to the new chain root,
  increments `catalog_generation`, and includes the new chain pages in
  `page_count`.
- Keep rollback journal coverage unchanged. Because old catalog chains are
  immutable, restoring the old header and old root page plus truncating the
  tail removes or hides the newly appended catalog chain.
- Do not split an individual record across pages in this slice. A single record
  larger than one page payload remains unsupported.
- Loosen catalog record validation so blob and index-root pages may live before
  or after the current catalog root. A referenced page is valid when it is
  nonzero and below `header.page_count`; the blob or leaf reader remains
  responsible for validating its own page type.

## Compatibility Impact

SQL-visible metadata behavior should be unchanged except that larger schemas no
longer fail solely because the catalog root page is full. Discovery callbacks
continue to report the same schema/table/FK/index metadata from the primary
file.

## DDL Metadata Routing Impact

All existing durable routed DDL metadata record types must participate in the
same chain:

- schema namespace records;
- table-definition records;
- foreign-key metadata records;
- index-root records.

Partial support would make mixed schemas fail unpredictably when a later
rename, drop, FK lookup, or index-root lookup scans only one page.

## Single-File And Lifecycle Impact

Catalog pages remain durable pages inside the primary `.mylite` file. Old
catalog chains become unreachable append-only history until free-space
management exists. No new persistent sidecar is introduced.

Recovery keeps the existing rollback-journal lifecycle. New catalog pages are
written before the header that makes them reachable, so a crash before header
publication leaves unreachable tail pages that recovery truncates. A crash
after header publication is protected by the existing journal completion rules.

## Public API And File-Format Impact

No public C API change. The existing file-format version stays valid while
previously reserved catalog linkage becomes active. Existing single-page
catalog files remain readable.

## Storage-Engine Routing Impact

Routed `InnoDB`, `MyISAM`, `Aria`, `MEMORY` / `HEAP`, and `BLACKHOLE` metadata
all use the same MyLite catalog helpers, so the chain is engine-neutral.
Unsupported server-oriented objects remain out of scope.

## Wire-Protocol And Integration Impact

No wire-protocol package changes. Protocol-facing metadata will observe the
same MariaDB table discovery and information-schema hooks after storage reads
from a multi-page catalog chain.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to owned catalog-image helpers,
chain validation, chain encoding, and tests.

## Tests And Verification

- Add storage unit coverage that creates enough table metadata to require more
  than one catalog page.
- Verify table existence and table-definition reads across the first and last
  records in a multi-page catalog chain.
- Keep existing schema, table-listing, FK, index-root, drop, and rename storage
  tests on the same catalog-image helpers so each record type is covered by the
  chain-capable implementation.
- Verify a failed statement rolls back from a newly published multi-page
  catalog to the statement-start catalog.
- Verify recovery from a catalog-publication journal truncates the newly
  appended chain and preserves the old catalog view.
- Run storage tests, storage-smoke tests, compatibility-harness storage groups,
  formatting checks, and `git diff --check`.

## Acceptance Criteria

- Catalog metadata is no longer limited to one 4 KiB root page.
- All current catalog record types can be found, listed, appended, removed, and
  renamed across chain pages.
- Catalog mutations append a fresh chain and never require in-place updates to
  old overflow pages.
- Existing single-page catalog files remain valid.
- Recovery and statement rollback preserve the same durable metadata semantics
  as before the chain.

## Risks

- Old catalog chains are not reclaimed until free-space management lands.
- One-record-per-page-payload remains a hard limit, which is acceptable for
  current records because large table and FK payloads already live in blob
  pages.
- The statement and read caches still cache only the validated root page; chain
  reads may need a broader catalog-image cache later if metadata-heavy workloads
  make catalog scanning hot again.
