# Prepared Insert Lazy Leaf Page Order

## Problem

The sorted leaf-page order fast path avoids allocating an order array when an
entryset is already sorted. Fresh prepared-insert profiling still shows
`prepare_index_leaf_pages()` spending time in both the sortedness scan and the
fallback `build_raw_index_entry_order()` path.

For out-of-order entrysets, the current code first scans adjacent entries to
find that sorting is needed, then allocates and insertion-sorts from the
beginning. That repeats comparison work in the branch snapshot/refold hot path.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `prepare_index_leaf_pages()` is the sampled maintained-index insert hotspot.
- `encode_index_leaf_page()` can already encode directly from entryset order
  when it receives a `NULL` order pointer.
- `build_raw_index_entry_order()` is still needed by split, range, root, and
  limited-entryset paths that require a full order array unconditionally.

## Design

Replace the full sortedness scan in `prepare_index_leaf_pages()` with a lazy
order builder:

- Walk adjacent raw entries once.
- Return a `NULL` order pointer when the entryset is already ordered.
- At the first inversion, allocate identity order, then continue insertion-sort
  from that inversion index.
- Leave `build_raw_index_entry_order()` unchanged for callers that always need
  an order array.

This preserves the previous no-allocation sorted path while avoiding a separate
pre-scan before sorting out-of-order leaf snapshot inputs.

## Compatibility Impact

No SQL-visible, API-visible, storage-routing, or file-format behavior changes.
Leaf pages remain encoded in raw key and row-id order.

## Single-File And Lifecycle Impact

No checkpoint, transaction, journal, lock, recovery, or companion-file changes.
This slice only changes transient ordering work before existing page writes.

## Binary-Size And Dependency Impact

No dependency changes. The added helper is small first-party storage code.

## Test And Verification Plan

- Reuse the storage hook coverage for sorted and unsorted full leaf-page
  preparation.
- Keep storage and storage-smoke tests passing.
- Run `git diff --check`, `git clang-format --diff`, and a prepared-insert
  component benchmark.

## Acceptance Criteria

- Sorted full leaf-page preparation still skips order allocation.
- Unsorted full leaf-page preparation still allocates exactly one order and
  produces sorted leaf bytes.
- The fallback full-order helper remains available for callers that require it.
- Relevant tests and formatting checks pass.

## Risks

The lazy helper assumes the prefix before the first inversion is already sorted
by the same comparison used by insertion sort. That follows directly from the
adjacent scan; if a caller later supplies invalid entryset arrays, existing
fixed-key and bounds validation still guard durable page encoding.
