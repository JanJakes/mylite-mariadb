# Maintained Index Root Overflow Tail Start

## Problem

Overflow-marked maintained roots currently scan the append tail from
`root_page + 1`. That is correct, but the first overflow insert writes row
payload pages before the fallback index-entry pages. Large rows can therefore
force every overflow-root exact or full index read to re-read payload pages that
cannot contribute index entries or row-state visibility.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. MariaDB still reaches these paths
  through handler row writes and index reads.
- `mylite_storage_append_row_with_index_entries()` writes row payload pages,
  then rewrites maintained roots, then appends fallback index-entry pages for
  still-changed index entries.
- `write_maintained_index_root_overflow_flags()` marks full maintained roots
  before fallback index-entry pages are published.
- `read_index_leaf_run_root()` derives maintained-root `tail_page_id` from
  `root_page + 1` when `HAS_OVERFLOW_TAIL` is set.
- `refill_maintained_index_root_from_overflow_tail()` uses the same
  `root_page + 1` start when rebuilding a compact live entryset after deletes.

## Scope

- Store the first fallback index-entry page id in maintained roots when an
  insert first overflows a full root.
- Use that stored tail start for root-backed exact/full reads and delete refill.
- Preserve compatibility for overflow roots that have the flag but no stored
  tail start by falling back to `root_page + 1`.

## Non-Goals

- No root split, B-tree navigation, free-list reclamation, or tail truncation.
- No catalog root update during row DML.
- No attempt to skip row payload pages after the first overflow index-entry
  page. That needs a real index page chain or navigable structure.
- No public API change.

## Design

Use the unused bytes between the maintained-root `flags` field and the payload
start for an optional `overflow_tail_page_id`. A value of `0` means legacy or no
explicit start. When `HAS_OVERFLOW_TAIL` is set and the stored page id is
nonzero, readers start overlay scans there; otherwise they use `root_page + 1`.

The append path already knows the first fallback index-entry page id after row
payload pages are written and before `write_index_entry_pages()` publishes
entries. Thread that page id into overflow-root marking and store it when a root
does not already have a tail start. Existing overflow roots keep their original
start, because later overflow inserts may append more entries but must not move
the scan start forward past older fallback entries.

The decode path should accept:

- no overflow flag and `overflow_tail_page_id == 0`;
- overflow flag and `overflow_tail_page_id == 0` for older roots; and
- overflow flag with a page id inside the current file.

Any nonzero tail start without the overflow flag is corrupt.

## Compatibility Impact

SQL-visible behavior remains unchanged. This is an internal read-path
optimization for MyLite-routed durable tables.

## Single-File And Lifecycle Impact

The root page uses previously unused bytes in the existing primary `.mylite`
file page. No companion files are introduced. Dirty root writes remain covered
by the existing statement or transaction journal protection for maintained-root
mutations.

## Public API And File-Format Impact

No public API change. The maintained-root page layout gains a known optional
field in bytes that were previously zeroed and checksummed. Decoding remains
strict enough to reject impossible flag/tail combinations.

## Storage-Routing Impact

Durable MyLite-routed indexes avoid scanning first-overflow row payload pages
when reading overflowed maintained roots. Volatile engines are unaffected.

## Binary-Size, License, And Dependency Impact

No imported code or dependency change. Binary impact is limited to small
first-party storage field handling.

## Test Plan

- Extend maintained-root overflow-tail coverage so the overflow row has a large
  payload and the stored tail start points at the fallback index-entry page
  after the payload pages.
- Verify exact lookup, indexed-row lookup, full index reads, update, delete,
  and refill still behave correctly with the stored tail start.
- Add decode coverage for impossible nonzero tail start without the overflow
  flag if an existing test helper can corrupt root bytes locally.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- New overflow roots record the first fallback index-entry page id.
- Roots with existing overflow flags and no stored start remain readable through
  the `root_page + 1` fallback.
- Reads and refill use the stored start when present.
- Existing maintained-root rollback, recovery, overflow-tail, and refill
  coverage remains green.

## Implementation Evidence

- Added `MYLITE_STORAGE_FORMAT_INDEX_ROOT_OVERFLOW_TAIL_PAGE_OFFSET` in the
  maintained-root reserved header space.
- Overflow-root marking stores the first fallback index-entry page id the first
  time a root overflows and leaves existing overflow starts unchanged.
- Maintained-root readers and refill use the stored start when present and
  preserve the `root_page + 1` fallback for zero-valued legacy starts.
- Decode rejects nonzero tail starts without the overflow flag and tail starts
  outside the current file.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c`

## Risks And Open Questions

- This only skips the first row-payload span. It does not replace the planned
  multi-page navigable index or a per-index append chain.
- The field reuses reserved root-page bytes without a format-version bump; that
  is acceptable only because older roots had those bytes zeroed and checksummed.
