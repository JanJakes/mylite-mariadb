# Unique Index Row Fetch Fast Path

## Problem

Prepared primary-key point selects are now much faster than direct SQL, but the
handler still performs two durable storage calls for a unique exact lookup:
first it resolves the index key to a row id, then it immediately asks storage
to fetch that row payload. The second call reopens or revalidates the same
primary file and catalog state that the first call just read.

That split is correct, but it is not SQLite-like. A primary-key point read
should use one storage pass to resolve the index entry and materialize the row
payload.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()` has
  a durable raw exact unique-key branch that calls
  `mylite_storage_find_index_entry()`, allocates one cursor entry, then calls
  `materialize_index_cursor_rows()`.
- `materialize_index_cursor_rows()` calls
  `mylite_storage_read_indexed_rows()` for the just-resolved row id, which
  repeats open/header/catalog work before reading the payload.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  already has the file, header, catalog page, table id, index caches, and leaf
  metadata needed to read the row payload safely before closing the file.

## Design

- Add `mylite_storage_find_indexed_row()` to the first-party storage API. It
  resolves an exact index key to one row id and returns that row payload in the
  same durable file operation.
- Keep existing lookup behavior:
  - use active exact-index cache, published leaf roots, durable exact-index
    cache, then append-log scan in the same order as
    `mylite_storage_find_index_entry()`;
  - return `MYLITE_STORAGE_NOTFOUND` when no row id matches;
  - report a resolved index entry whose row payload cannot be read as
    corruption, not as a normal key miss;
  - mark a found row live in the active read statement cache;
  - store row payloads in the existing durable row-payload cache.
- Route only non-volatile fixed-record raw exact unique-key cursor
  construction through the new API. Volatile rows, BLOB/TEXT rows, and
  non-unique entrysets keep the current paths.
- Preserve the existing handler cursor representation by filling the single
  cursor entry and attaching one materialized row payload to `index_rows`.

## Compatibility Impact

No SQL behavior changes. The branch is only selected for fixed-record exact
full-key unique lookups that already materialize exactly one row or no row.
BLOB/TEXT tables retain the existing two-call materialization path until their
row-payload lifetime is reviewed separately.

## Single-File And Lifecycle Impact

No file-format, sidecar, journal, or lifecycle change. The fast path reads the
same durable `.mylite` pages as the previous two-call path.

## Public API And File-Format Impact

The first-party storage package gains one C API function. The public
`libmylite` API and `.mylite` format do not change.

## Storage-Engine Routing Impact

`ENGINE=InnoDB` and other routed durable engines continue to resolve to MyLite.
The optimization sits below engine routing in the MyLite handler.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to one small storage function
and handler branch.

## Test And Verification Plan

- Add storage unit coverage for successful indexed-row lookup, missing key,
  misuse inputs, and stale old-key lookup after update/delete.
- Run storage unit tests.
- Run storage-engine compatibility smoke.
- Run the local performance baseline to compare primary-key point selects.
- Run `git diff --check`.

## Acceptance Criteria

- Fixed-record durable unique exact index cursor construction no longer
  performs a second storage API call solely to materialize the found row.
- Existing storage and storage-engine compatibility tests pass.
- The benchmark still validates primary-key point-select checksums.

## Risks

- This reduces durable storage-layer overhead; it does not remove MariaDB SQL
  parse, optimizer, prepared-statement reset, or result-materialization cost.
  SQLite-like API latency still requires additional prepared-path and storage
  work.
- BLOB/TEXT exact unique lookups are deliberately left on the old
  materialization path in this slice because the broader handler path exposed
  row-lifetime sensitivity in compatibility smoke tests.
