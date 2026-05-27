# Packed Index Entry Cache Set

## Problem

Packed append-tail index-entry page version `2` can store many fixed-key-size
entries in one page, but the active insert writer currently remembers only one
appendable packed index-entry page. The prepared insert component benchmark
uses two indexes, so inserts alternate between primary and secondary key
entries and replace the single cache on every changed entry.

A kept 100,000-row benchmark file after packed v2 entry pages still had:

- `63,476` index-entry pages;
- `65,329` logical index-entry cells;
- `63,473` version-`2` index-entry pages.

That proves the page format works but the writer cache is too narrow for the
common multi-index insert shape.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:3929-4086` prepares all MariaDB key
  images for a row and passes them together to
  `mylite_storage_append_row_with_index_entries()`.
- `mariadb/storage/mylite/ha_mylite.cc:3628-3699` materializes rows later
  from the opaque handler row reference; this slice does not change row
  reference semantics.
- `packages/mylite-storage/src/storage.c::write_packed_index_entry_pages()`
  currently stores the last appendable packed index-entry page in one
  statement cache.
- `packages/mylite-storage/src/storage.c::packed_index_entry_append_page_count()`
  mirrors that single-cache behavior for page reservation and row-id
  prediction.
- Append-tail scans process pages in physical order and filter by table id,
  index number, key width, and row-state visibility. A later page for another
  index does not affect visibility for the cached index shape, but a later
  row-state, maintained root, leaf, branch, or same-shape index-entry page
  must keep blocking old-page appends to preserve ordering.

## Design

- Replace the single packed index-entry append cache with a small fixed-size
  statement-local cache set keyed by:
  - catalog table id;
  - MariaDB index number;
  - fixed key size.
- Keep one cache entry per shape and update it when a new v2 page is allocated
  or appended.
- Let cached v2 pages remain appendable across later buffered row pages and
  later packed index-entry pages for other shapes.
- Reject reuse when any later buffered page is:
  - a row-state page;
  - an index root, leaf, or branch page;
  - an append-tail index-entry page for the same shape;
  - an unknown or unbuffered page.
- Make `packed_index_entry_append_page_count()` simulate the same cache set so
  page reservation, maintained-index row-id planning, and actual writes agree.
- Keep oversized keys on the legacy v1 one-entry page path.

## Non-Goals

- No public API, SQL, MariaDB handler, or storage-engine routing change.
- No change to v2 page format.
- No persistent cache or durable metadata.
- No final B-tree cleanup of append-tail overlays.

## Compatibility Impact

SQL-visible behavior is unchanged. `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, and default routed durable tables still resolve to MyLite
storage under the embedded profile. The change only reduces internal page
volume for active fixed-size multi-index inserts.

## Single-File And Embedded Lifecycle Impact

All durable state remains in the primary `.mylite` file. The cache set is
statement-local transient state and is cleared with the existing append-buffer
cleanup paths. Rollback continues to restore packed page entry counts and
payload prefixes through buffered-page undo.

## Public API And File-Format Impact

No public API or file-format change. Existing v1 and v2 index-entry pages stay
readable.

## Storage-Engine Routing Impact

No routing policy change. Routed durable tables benefit when inserts produce
multiple append-only index-entry shapes in one row.

## Binary-Size, License, And Dependency Impact

Small first-party storage changes only. No dependency or license change.

## Test And Verification Plan

- Extend active packed indexed insert coverage so two append-only index shapes:
  - share one packed row page for initial rows;
  - each share one packed v2 index-entry page;
  - remain visible through exact lookup before and after commit;
  - restore both packed index-entry pages after nested rollback;
  - do not append to an old same-shape page after a row-state page.
- Reinspect the prepared insert component benchmark file and confirm the
  index-entry page count drops materially for the two-index workload.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Multi-index active inserts keep one appendable packed v2 page per active
  index shape.
- Page-count prediction and actual page writes stay aligned.
- Same-shape append-tail ordering is preserved across row-state and maintained
  index pages.
- Existing v1 and v2 index-entry readers remain compatible.
- Existing storage and routed storage-engine tests pass.

## Verification Results

On the local `storage-smoke-dev` build, the kept
`prepared-insert-components 1000 100000` benchmark file changed from the
pre-slice shape of `63,476` index-entry pages and `290,775,040` bytes to:

- `215` index-entry pages;
- `65,329` logical index-entry cells;
- `214` version-`2` index-entry pages;
- `31,653,888` final bytes;
- `7,728` header pages;
- `22.622 us/op` for the prepared insert step component;
- `19.196 ms` for prepared insert commit.

The same benchmark still reported only two branch tail overlay scans and `48`
branch tail overlay scan reads.

## Risks And Unresolved Questions

- The initial cache set is intentionally bounded. Workloads with more active
  index shapes than the bound can still allocate additional v2 pages.
- This still leaves append-tail scans in place. It reduces write volume and
  file size, but navigable index maintenance remains separate roadmap work.
