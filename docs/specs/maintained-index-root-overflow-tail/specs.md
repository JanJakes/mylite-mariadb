# Maintained Index Root Overflow Tail

## Problem

Single-page maintained index roots can become full. When that happens, the
current insert planner deliberately leaves the append-only index-entry write
enabled for the overflowing row. Before this slice, root-backed exact and full
index readers treated maintained roots as fully authoritative and skipped the
append tail, so a row that overflowed a full maintained root could become
invisible to root-backed index reads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines the storage-engine contract for
  `write_row()`, `update_row()`, `delete_row()`, and `index_read*()` entry
  points. MyLite must return index-visible rows through the same handler
  contract regardless of whether an entry lives in a maintained root or an
  append-only fallback page.
- `mariadb/sql/handler.cc::handler::ha_write_row()` and
  `handler::ha_index_read_map()` delegate row writes and index reads to the
  engine implementation.
- `packages/mylite-storage/src/storage.c::plan_maintained_index_root_inserts()`
  skips maintained-root insertion when
  `root_page.entry_count >= maintained_index_root_entry_capacity()`. The
  corresponding `index_entry_changed` slot remains enabled, so
  `write_index_entry_pages()` appends a fallback index-entry page for that row.
- Before this slice,
  `packages/mylite-storage/src/storage.c::read_index_leaf_run_root()` marked
  maintained-root runs with `tail_page_id = header->page_count`.
- `read_index_leaf_exact_entries()`, `read_index_leaf_exact_row_ids()`, and
  `read_index_leaf_entries()` scan append-tail pages only when
  `tail_page_id < header->page_count`.

## Scope

- Preserve the current single-page maintained-root format.
- Make append-only fallback entries after a full maintained root visible to
  exact-entry, exact-entryset, indexed-row, and full-index reads.
- Preserve in-place maintained-root update and delete semantics for entries
  already present in the root.

## Non-Goals

- No root split, merge, B-tree navigation, or multi-page maintained root.
- No catalog format change.
- No SQL syntax or public API change.
- No broad rewrite of append-only row-state filtering.

## Design

Root-backed readers should keep using the maintained root as the base entry set
and scan pages appended after the root page as a visibility overlay. That is the
same high-level model already used by immutable leaf runs, but maintained-root
writers suppress duplicate append-only index entries for rows updated in place.
Therefore later row-state overlays do not need to mutate root entries that were
already rewritten in the root; they only need to keep fallback append-only
history consistent.

The minimal implementation is to make maintained-root runs expose their
append-tail start as `root_page + 1` again, then verify that the existing
forward append-tail scanners preserve the maintained-root cases:

- inserts that fit in the root add no duplicate index-entry page, so the tail
  contributes nothing for that key;
- inserts after the root is full append index-entry pages and become visible;
- updates/deletes for root-resident rows rewrite the root and append row-state
  pages, which should not add stale keys because the source row id is no longer
  present in the root;
- updates/deletes for fallback rows continue to use the existing append-only
  row-state overlay.

If that direct tail restoration fails an existing maintained-root rollback case,
the fallback is a narrower tail scan mode that ignores row-state overlays for
root-resident row ids while still applying them to append-only fallback entries.

## Compatibility Impact

No intentional SQL-visible behavior change except fixing missing index results
for rows inserted after a maintained root reaches capacity. This improves the
MySQL/MariaDB drop-in behavior expected from normal handler index reads.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. Overflow index entries
are ordinary append-only index-entry pages already covered by existing
statement, transaction, and recovery paths.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Routing Impact

MyLite-routed durable tables get correct root-plus-tail index visibility for
the current single-page maintained-root scope. MEMORY/HEAP and other volatile
paths are unaffected.

## Binary-Size, License, And Dependency Impact

No new dependency or imported code. Binary-size impact should be limited to a
narrow storage read-path change and tests.

## Test Plan

- Add storage unit coverage that publishes a maintained root exactly at its
  single-page capacity using a large fixed-width key, inserts one more row, and
  verifies:
  - the root metadata count remains at capacity;
  - the file grows by the row page plus one append-only index-entry page;
  - exact index-entry lookup sees the overflow key;
  - indexed-row lookup materializes the overflow row;
  - full index reads include both root and overflow entries.
- Extend the same test through update and delete of the overflow row to prove
  append-tail row-state overlays still hide stale fallback keys.
- Keep existing maintained-root insert, update, delete, rollback, and recovery
  tests passing.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Overflow rows appended after a full maintained root are visible through exact
  and full index read APIs.
- Maintained-root in-place insert, update, delete, rollback, and recovery
  coverage remains green.
- The fix does not add durable sidecars, catalog churn, or a new format.

## Initial Implementation

- Maintained-root leaf runs initially restored their append-tail overlay at
  `root_page + 1`, matching immutable leaf-run behavior. The follow-up
  [Maintained Index Root Overflow Tail Flag](../maintained-index-root-overflow-tail-flag/specs.md)
  slice narrows that scan to roots that have overflow history.
- The storage regression publishes a three-entry maintained root with
  `1330`-byte fixed keys, inserts a fourth row after the root is full, and
  verifies exact lookup, indexed-row lookup, and full index reads include the
  append-only overflow entry while root metadata remains at the root capacity.
  It then deletes a root-resident row, refills the root, updates and deletes the
  former overflow row, and verifies stale fallback keys stay hidden.

## Verification Results

- Initial regression failed before the read-path fix at
  `assert_index_entry_lookup()` for the overflow key.
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`

## Risks And Open Questions

- This still leaves root splits and multi-page maintained indexes for a later
  slice. Overflow tail scanning is correctness work, not the final performance
  structure.
- The direct tail restoration relies on current maintained-root writers
  suppressing duplicate index-entry pages for root-resident mutations. If later
  writers change that invariant, the read path will need a more explicit
  root-resident overlay guard.
