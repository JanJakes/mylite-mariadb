# Reusable Live Row Cache

## Problem Statement

The prepared primary-key update benchmark creates a nested checkpoint for every
row-DML savepoint. Exact-index row lookup marks the selected row as live and
validated in the active statement so update validation can avoid rereading row
visibility through `validate_direct_live_row()`. That cache is useful, but the
hot loop repeatedly allocates the same tiny live-row cache entry and row-id
arrays, then frees them during nested statement cleanup.

The next performance slice should keep the visibility shortcut while removing
that allocation/free churn from ordinary single-row prepared updates.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), as recorded in
  `docs/architecture/engineering-standards.md`.
- MyLite first-party storage code owns the relevant behavior in
  `packages/mylite-storage/src/storage.c`.
- `find_indexed_row_payload()` and row-read paths call
  `mark_active_live_row()` / `mark_active_validated_live_row()` after selecting
  a row through index lookup.
- `validate_direct_live_row_in_statement()` checks
  `active_validated_live_row_known_in_statement()` and
  `active_live_row_known_in_statement()` before falling back to a physical row
  visibility read.
- `append_live_row_cache()` creates a cache-entry array with initial capacity
  `4`; `add_live_row_id()` and `add_validated_live_row_id()` create row-id
  arrays with initial capacity `16`.
- `free_statement()` currently calls `clear_live_row_caches()` for every nested
  statement, and `clear_live_row_caches()` frees the cache-entry array and any
  row-id arrays immediately.
- The storage layer already has a bounded thread-local reuse pattern for
  buffered page undo storage through `reusable_buffered_page_undos`,
  `adopt_reusable_buffered_page_undos()`, and
  `release_buffered_page_undos()`.

## Proposed Design

Retain one small cleared live-row cache set per thread:

- Add named initial-capacity constants for live-row cache entries and live-row
  row-id arrays.
- Add a thread-local `reusable_live_row_caches` set.
- When a statement appends its first live-row cache, adopt the reusable set
  before allocating a new entry array. If the retained metadata matches the
  requested table, reuse that entry directly.
- On statement cleanup, release live-row caches through a helper that either:
  - resets row-id and validated-row counts and moves one small cache set into
    thread-local storage, or
  - frees the cache set normally.
- Retain only small cache sets:
  - at most one cache entry,
  - cache-entry capacity no larger than the initial live-row cache capacity,
  - row-id and validated-row-id capacities no larger than the initial row-id
    capacity.
- Keep the cache entry metadata while clearing row-id counts. Reusing metadata
  lets the common same-table prepared update avoid both cache-entry and row-id
  array allocation. If the next statement targets different table metadata, the
  cache appends a second entry and later frees normally instead of retaining a
  broader set.

## Affected Subsystems

- First-party MyLite storage runtime only.
- Nested checkpoint allocation and statement cleanup.
- Active live-row validation cache maintenance.

## Compatibility Impact

No SQL, C API, storage-engine routing, DDL metadata, wire-protocol, or file
format behavior changes. This is an in-process allocation strategy for
best-effort active-statement cache storage.

## Single-File And Embedded Lifecycle

No durable file or companion-file behavior changes. The retained state is
thread-local process memory only and must not carry row visibility facts across
statements because row-id counts are cleared before reuse.

## Binary Size, License, And Dependencies

The change adds a few first-party helpers and no dependencies. Binary-size impact
should be negligible.

## Test And Verification Plan

- Build storage-smoke targets:
  `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run the focused storage and embedded tests.
- Run the full `storage-smoke-dev` CTest suite.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared-update performance baseline.
- Capture a focused sample if the performance result needs source-level
  confirmation.

## Acceptance Criteria

- Small live-row cache sets are retained and adopted without allocation on the
  next matching live-row cache append.
- Row-id and validated-row-id counts are always cleared before reuse.
- Larger or multi-table cache sets free normally.
- Existing storage, embedded lifecycle, and storage-engine tests pass.
- Prepared-update performance is neutral or improved.

## Risks And Open Questions

- Retaining stale metadata is intentional, but stale row-id counts would be a
  correctness bug. The implementation must reset both `count` and
  `validated_count` before publishing the reusable set.
- A future multi-table hot path may need a broader reuse policy, but this slice
  intentionally retains only the ordinary single-table prepared update shape.
