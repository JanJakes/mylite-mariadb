# Deep Branch Cursors

## Problem

Branch-root readers can recurse through lower branch pages, but they still
flatten every reachable leaf page into fixed-size arrays capped at one branch
page's maximum child count. That keeps large multi-level snapshots on the
append-history fallback even when the branch tree itself is valid.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines the handler index cursor contract through
  `index_read_map()`, `index_next()`, and `index_next_same()`.
- `mariadb/sql/handler.cc` wraps those handler calls in
  `handler::ha_index_read_map()` and `handler::ha_index_next()`, updates
  table status and read statistics, and leaves physical index navigation to the
  storage engine.
- `mariadb/storage/mylite/ha_mylite.cc` builds ordered MyLite index cursors
  through storage-level full, exact, and prefix entry reads, then materializes
  matching rows through the handler.
- `packages/mylite-storage/src/storage.c` already validates branch pages,
  recursively follows lower branch pages for exact and prefix lookup, and
  collects branch leaves for full scans. The current collection structs store
  leaf page ids in fixed arrays sized by
  `MYLITE_STORAGE_INDEX_BRANCH_MAX_CHILDREN`, which makes otherwise valid
  deeper trees return `MYLITE_STORAGE_UNSUPPORTED`.

## Design

- Replace fixed branch leaf collection with a small-inline vector that grows
  when a branch snapshot has more leaves than fit in one branch page.
- Store the flattened leaf page ids in the returned leaf-run descriptor with
  the same small-inline shape so existing scan, exact, prefix, and
  prefix-exists readers can keep resolving page offsets without a second branch
  walk.
- Preserve branch-page validation: every internal branch child must remain the
  next lower level, match table/index/key metadata, and have a high fence that
  matches the parent cell.
- Keep append-tail overlay semantics unchanged. The overlay starts after the
  highest page id in the static branch subtree, not after the last physical
  contiguous leaf page.

## Non-Goals

- No new branch publication algorithm. This slice only reads deeper snapshots.
- No new branch mutation support above the existing maintained single-level and
  bounded root-split paths.
- No branch-page merge, redistribution, or file compaction.

## Compatibility Impact

SQL and handler behavior do not change except that larger published MyLite
branch snapshots can serve the same ordered, exact, and prefix reads that
smaller snapshots already serve. MariaDB's handler cursor contract remains the
compatibility boundary.

## Single-File And Lifecycle Impact

No file-format change. Durable state stays in the primary `.mylite` file. The
larger flattened leaf list is transient process memory owned by one storage
read operation and freed before the operation returns.

## Public API, Storage Routing, And Wire Protocol

No public `libmylite` API, storage-engine routing, or wire-protocol changes.

## Binary Size And Dependencies

Small first-party vector helpers only. No new dependency and no meaningful
binary-size impact.

## Tests And Verification

- Add a storage hook test that synthesizes a valid level-2 branch tree whose
  leaf count exceeds the former fixed-array bound, then verifies full scans,
  exact lookup, prefix reads, prefix-exists checks, append-tail overlay reads,
  and unrelated-leaf corruption avoidance.
- Run focused storage tests, storage smoke, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Valid deep branch snapshots with more than
  `MYLITE_STORAGE_INDEX_BRANCH_MAX_CHILDREN` leaves no longer return
  `MYLITE_STORAGE_UNSUPPORTED`.
- Ordered full, exact, prefix, and prefix-exists readers resolve leaf page ids
  through the deep branch tree and preserve overlay visibility.
- Corrupt branch metadata or impossible leaf counts still return corruption
  instead of being silently accepted.
- Docs describe deep branch cursor support without claiming broader branch
  mutation or compaction support.

## Risks

- Full-index scans still materialize the flattened leaf list, so extremely
  large indexes may allocate proportional transient memory until a true cursor
  pager walks the branch tree incrementally.
- Deep branch mutation support remains intentionally outside this slice.
