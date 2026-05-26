# Prepared Insert Sorted Leaf Page Order

## Problem

Prepared insert profiling shows `prepare_index_leaf_pages()` spending a
meaningful part of the maintained-index insert path in
`build_raw_index_entry_order()`.

The branch refold path often starts from already-sorted leaf entries and appends
a monotonic insert key. Rebuilding an explicit order array for those cases adds
allocation and insertion-sort work before encoding bytes that can already be
read in entryset order.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `prepare_index_leaf_pages()` validates a fixed raw key width, computes leaf
  capacity, builds a raw entry order, and then calls `encode_index_leaf_page()`
  for each leaf page.
- `build_raw_index_entry_order()` fills an identity order and insertion-sorts
  by raw key bytes and row id.
- `encode_index_leaf_page()` only needs an order indirection when the entryset
  is not already sorted by the same raw key and row-id order.

## Design

Add a sorted-entryset check in `prepare_index_leaf_pages()`:

- If adjacent raw entries are already ordered, pass a `NULL` order pointer to
  `encode_index_leaf_page()` and let the encoder use identity order.
- If any adjacent pair is out of order, keep the existing order-array build and
  sort path.
- Keep existing fixed-key validation, leaf capacity checks, page layout, and
  checksums unchanged.

This deliberately limits the first fast path to full leaf-page preparation,
which is the sampled prepared-insert branch snapshot/refold hotspot.

## Compatibility Impact

No SQL-visible, API-visible, or storage-routing behavior changes. Encoded leaf
pages are byte-equivalent for already-sorted entrysets and remain sorted for
unsorted entrysets.

## Single-File And Lifecycle Impact

No file-format, checkpoint, transaction, journal, lock, or companion-file
changes. The slice only removes an in-memory ordering allocation for a trusted
pre-encoding shape.

## Binary-Size And Dependency Impact

No dependency changes. The added branch is small first-party storage code.

## Test And Verification Plan

- Add a storage hook test that prepares a sorted entryset without calling
  `build_raw_index_entry_order()`.
- Assert the same helper still builds an order for unsorted input and encodes
  the expected sorted leaf bytes.
- Run storage unit tests, storage-smoke embedded tests, formatting checks, and
  the prepared-insert component benchmark.

## Acceptance Criteria

- Sorted full leaf-page preparation skips explicit order construction.
- Unsorted full leaf-page preparation still sorts by raw key and row id.
- Encoded leaf pages keep the existing durable format and checksum semantics.
- Relevant tests and formatting checks pass.

## Risks

The fast path relies on the same adjacent comparison as the existing insertion
sort. If future callers introduce variable-width key entrysets here, the
existing fixed-key validation still rejects them before the identity-order
decision.
