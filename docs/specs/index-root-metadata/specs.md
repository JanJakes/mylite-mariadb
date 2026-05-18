# Index Root Metadata

## Problem

MyLite index reads are still based on append-only index-entry scans. B-tree or
equivalent navigable index pages need stable per-index root pointers, but the
current catalog only records schemas, table definitions, and foreign keys. A
storage-level index tree cannot be published safely until root page metadata has
an explicit catalog home and follows the same atomic catalog publication rules
as table definitions.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` calls
  `ha_index_read_map()` for range starts, and
  `handler::read_range_next()` delegates equality continuation to
  `ha_index_next_same()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` and
  `index_read_idx_map()` build MyLite index cursors for the active key number.
  Any future root lookup must be keyed by MariaDB's key number and preserve
  handler-ordered cursor behavior.
- `packages/mylite-storage/src/storage_format.h` defines catalog record types
  for table definitions, schemas, and foreign keys only. The generic catalog
  record header already has a table id, root-page field, size/generation field,
  and flags field, but there is no index-root record type.
- `packages/mylite-storage/src/storage.c::mylite_storage_drop_table()` removes
  child FK records and the table record through a rollback-journal-protected
  catalog publication. Index-root records must be removed in the same catalog
  publication as the table they belong to.
- `packages/mylite-storage/src/storage.c::rename_table_record()` rewrites table
  and FK records on table rename. Index-root records must follow the owning
  table identity without changing the table id or root page.
- Existing row and index-entry mutations append pages and then publish the
  header page count. Catalog metadata changes additionally journal and rewrite
  the catalog root page. Updating an index root must use the catalog-publication
  path, not a hidden side channel.

## Design

- Add a new catalog record type for index roots. The record is keyed by
  schema name, table name, table id, and MariaDB key number.
- Store the MariaDB key number in the record flags field, the root page in the
  existing root-page field, and the indexed entry count or tree generation in
  the existing size field. Keep the table id as the durable owner identity.
- Add first-party storage APIs to store, read, and drop index roots. These APIs
  are intentionally about metadata only; they do not define B-tree page layout.
- Preserve catalog lifecycle:
  - table drop removes index-root records for the dropped table,
  - table rename rewrites index-root records to the new schema/table name while
    preserving table id and root page,
  - schema drop removes index-root records through the existing schema-wide
    record removal path,
  - missing index-root records return `MYLITE_STORAGE_NOTFOUND`.
- Keep append-only index-entry pages as the read path for now. The next storage
  navigation slice can introduce index root and leaf page formats that use this
  metadata.

## Compatibility Impact

No SQL-visible behavior changes in this slice. The metadata is an internal
storage prerequisite for future B-tree navigation and must not change MariaDB
key comparison, cursor ordering, duplicate checks, or existing handler routing.

## Single-File And Lifecycle Impact

Index-root metadata lives in the primary `.mylite` catalog root page and is
covered by the existing rollback journal when catalog metadata is republished.
No sidecar files, dynamic plugins, or external durable engine files are
introduced.

## Public API And File-Format Impact

- File format gains a catalog record type for index-root metadata while keeping
  the format version unchanged during early development.
- `mylite_storage_get_capabilities()` gains an index-root metadata capability.
- The first-party storage API gains store/read/drop functions and small
  metadata structs. These are storage-internal building blocks, not the public
  `libmylite` SQL API.

## Storage-Engine Routing Impact

No routing change. The metadata belongs to MyLite-routed durable tables only.
Runtime-volatile MEMORY/HEAP rows and indexes remain process-local and do not
publish durable root records.

## Binary-Size And Dependency Impact

No dependency change. Binary impact is limited to catalog record helpers and
unit tests.

## Tests And Verification

- Add storage unit tests for store/read/drop index root records.
- Cover table rename preserving index-root ownership and root values.
- Cover table drop removing index-root records with the owning table.
- Cover schema drop removing index-root records through the schema-wide catalog
  path.
- Cover invalid inputs, missing records, and catalog validation for the new
  record type.
- Run storage unit tests, changed-line formatting checks, and `git diff --check`.
- Local verification on this branch:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `tools/mylite-compat-harness run storage-engine`
  - `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `git diff --check`

## Acceptance Criteria

- Index root records have an explicit catalog record type and validation rules.
- First-party storage APIs can publish, read, replace, and drop per-index root
  metadata.
- Table rename and table drop keep index-root metadata consistent.
- Existing row/index storage behavior and storage unit tests continue to pass.

## Risks

- The current single-page catalog limits how much metadata can fit before
  multi-page catalog work exists. Index root records should be compact and
  future B-tree page work should not create one catalog record per leaf page.
- This slice does not implement B-tree pages or improve lookup asymptotics by
  itself; it only creates the root publication path needed for that work.
