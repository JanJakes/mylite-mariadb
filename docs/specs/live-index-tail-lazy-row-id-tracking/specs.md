# Live Index Tail Lazy Row-Id Tracking

## Problem Statement

Full live-index tail reads currently build a row-id tracking hash set from the
entire base entryset before scanning tail pages. That is necessary when row
state pages can remove or replace existing entries, but it is avoidable for the
common append-only tail used by prepared insert transactions. The latest
prepared-insert profile shows `track_index_entryset_row_ids()` as a visible hot
frame under branch-refold entryset construction.

This slice makes full live-index tail reads build the tracking set lazily, only
when a row-state page requires mutation of already collected entries.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are involved.
- `read_live_index_entries_from()` starts by calling
  `track_index_entryset_row_ids()` even when the scanned tail contains only
  index-entry pages.
- Insert-only prepared workloads append new row/index pages and do not need
  base row-id tracking unless a later row-state page appears.
- Existing tracked helpers still provide the correct removal/replacement
  behavior once a row-state page is observed.

## Design

- Add a lazy `has_tracked_row_ids` flag to `read_live_index_entries_from()`.
- Append matching index-entry pages directly while no row-state page has been
  observed.
- On the first relevant row-state page, build the tracking set from the current
  entryset, including any entries appended before that row-state page, then use
  the existing tracked removal/replacement helpers.
- Leave lower-bound and prefix-specific live scans unchanged in this slice
  because they apply additional filtering/removal rules and are not the current
  prepared-insert hot frame.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, file-format, or
wire-protocol behavior changes. Append-only tails produce the same logical
entryset; tails with row-state pages still build tracking before applying
removal/replacement semantics.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, locking, or companion-file changes.
This changes transient in-memory scan bookkeeping only.

## Binary Size And Dependencies

No dependency changes. Code-size impact is limited to a small control-flow
change and test-hook counter coverage.

## Test And Verification Plan

- Add a test hook counter for calls to `track_index_entryset_row_ids()`.
- Extend an append-tail branch read regression to verify full index reads over
  pure append tails do not build the tracking set.
- Run the storage test target and CTest selection.
- Run formatting checks, storage-smoke embedded storage-engine coverage, and
  prepared-insert component performance baselines.

## Acceptance Criteria

- `read_live_index_entries_from()` skips initial row-id tracking for append-only
  tails.
- Existing row-state removal/replacement behavior remains routed through the
  tracked helpers after the first row-state page.
- The append-tail regression fails against eager tracking and passes with lazy
  tracking.
- Targeted tests and checks pass.

## Risks

- The main risk is delaying tracking past an append that later needs to be
  removed by a row-state page. Building tracking from the current entryset at
  the first row-state page covers both the original base entries and prior tail
  appends.
