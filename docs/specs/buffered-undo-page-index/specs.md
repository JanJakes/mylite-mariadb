# Buffered Undo Page Index

## Problem

Prepared updates in one transaction repeatedly call
`capture_buffered_page_undo_with_used_size()` for row and index pages that may
already have undo images captured earlier in the same statement/transaction.
The current duplicate check scans `statement->buffered_page_undos.entries`
linearly.

For repeated updates over a stable row set, the undo list grows early and then
every later update pays a duplicate scan before it can skip capture. The sampled
profile now shows `capture_buffered_page_undo_with_used_size()` after the
smaller inline and statement-lookup slices.

## Source Findings

- MariaDB base line: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite-owned source: `packages/mylite-storage/src/storage.c`.
- `mylite_storage_buffered_page_undo_list` stores undo entries in insertion
  order, and rollback restore iterates that entry array in order.
- `capture_buffered_page_undo_with_used_size()` only needs a membership check by
  `page_id` before appending a new undo entry.
- Existing storage code already uses open-addressed row-id buckets for row-state
  maps, row payload caches, and buffered update rewrite shape caches.

## Design

Add a private open-addressed page-id bucket index to
`mylite_storage_buffered_page_undo_list`:

- keep `entries` as the source of rollback order and undo bytes;
- add buckets keyed by `page_id` with a 1-based entry index, so zero remains
  the empty-bucket marker;
- keep the original linear check for small lists and use the bucket index once
  a statement has enough distinct undo pages for hashing to pay off;
- rebuild buckets when the bucket capacity grows;
- free bucket memory when the undo list is released;
- keep the small reusable undo-entry array, but do not reuse bucket memory; the
  small reusable list stays below the bucket threshold, avoiding stale bucket
  state and per-statement bucket allocation on tiny undo sets.

Do not change undo capture order, rollback restore order, page bytes, or
checksum-dirty restore behavior.

## Compatibility Impact

No SQL-visible behavior change.

## File And API Impact

No public API, file-format, or companion-file change.

## Storage Routing Impact

No routing change.

## Binary-Size Impact

Small code-size increase for private hash helpers and one bucket pointer in the
undo list. Runtime memory grows only while a statement has buffered page undos.

## Test And Verification Plan

- Build first-party storage smoke targets.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run prepared-update benchmark repeats to confirm tiny per-statement undo lists
  do not materially regress.
- Use sampling on future broad single-statement update workloads that cross the
  bucket threshold before claiming a hot-path win from this index.

## Acceptance Criteria

- Storage-smoke coverage remains green, including transaction/savepoint rollback
  coverage.
- Repeated undo capture by page id remains idempotent.
- Prepared-update timing does not regress materially.

## Risks

- The bucket index must not disturb rollback order. Entries stay append-only and
  restore continues to iterate the entry array.
- Reused small undo lists must not carry bucket state across statements.
