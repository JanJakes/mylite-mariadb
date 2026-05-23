# Free-List Root Append Coalescing

## Problem

Catalog and branch reclamation can coalesce a reclaimed run that immediately
precedes the current free-list root run. The opposite adjacent shape still
publishes a separate one-page or run node even when the reclaimed pages start
immediately after the current root run.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- Free-list pages validate with `page_id == run_start_page`, so an upward
  coalesced run must keep the current root page as the free-list root and
  rewrite its `run_page_count`.
- Rewriting the current free-list root dirties an existing page, unlike
  lower-adjacent prepend coalescing. The root page therefore must be included
  in the statement or recovery journal before it is updated.
- `encode_reclaimed_free_list_root_page()` centralizes current reclaim encoding
  for catalog runs and reclaimed branch leaves.

## Scope

- Detect when a reclaimed run starts immediately after the current free-list
  root run.
- Rewrite the current free-list root page with an extended `run_page_count`.
- Keep the current `free_list_root_page` unchanged for that higher-adjacent
  coalesced shape.
- Preserve existing lower-adjacent prepend coalescing and non-adjacent prepend
  behavior.
- Protect the current free-list root page before rewriting it in both catalog
  and branch reclamation paths.

## Non-Goals

- No arbitrary free-list-chain search or best-fit allocation.
- No coalescing with non-root free-list nodes.
- No file shrinking.
- No row or index page allocation from the free-list beyond existing catalog
  reuse.
- No file-format or public API change.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes the internal shape of the durable free-list in the primary `.mylite`
file.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Rollback and stale-journal
recovery restore the previous header, current free-list root page, and reclaimed
page bytes when a higher-adjacent coalescing attempt is not committed.

## File-Format Impact

No format-version change. The slice uses the existing free-list page layout.

## Test Plan

- Add storage-hook tests for catalog and branch reclamation where the reclaimed
  run starts immediately after the current free-list root run.
- Cover active-statement rollback for the catalog path so rewriting the current
  root page proves it is protected by the journal.
- Assert the header keeps the same `free_list_root_page`, the root run length is
  extended, and file size still matches the header.
- Preserve lower-adjacent coalescing tests and non-adjacent behavior.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Higher-adjacent reclaimed runs extend the current root run instead of adding a
  new free-list node.
- Lower-adjacent and non-adjacent reclaim behavior is unchanged.
- Recovery journaling protects every existing free-list root page rewritten by
  the coalescing path.
- Docs and roadmap distinguish root-adjacent coalescing from arbitrary-chain
  coalescing and allocator work.

## Risks And Follow-Ups

- Coalescing remains local to the current root run.
- The catalog allocator still consumes only the current root run.
