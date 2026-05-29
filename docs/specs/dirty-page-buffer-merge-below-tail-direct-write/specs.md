# Rejected Dirty-Page Buffer Merge Below-Tail Direct Write

## Problem

The current dirty-page buffer merge policy direct-writes protected existing
leaf pages and future-current index leaves with `0-31` free slots. Future
leaves with `32+` free slots still replay through the parent dirty buffer.
A raw `32-63` widening was rejected because it lost useful in-buffer
coalescing and regressed the prepared-insert step.

Tail-distance counters showed that the broad below-parent-max class contains
several distinct groups. This experiment tested one conservative group that was
neither near the current tail nor part of the very-far-behind class that still
records many replacements.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice records a rejected local experiment. No behavior code or upstream
  MariaDB source change is committed.
- `mylite_storage_commit_statement()` flushes top-level append and dirty-page
  buffers, but nested statements merge dirty-page undo coverage and then replay
  child dirty-page buffer entries into the parent through
  `merge_dirty_page_buffer()`.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  allows future-current index leaves to direct-write when the page id is below
  the parent statement's current header page count, outside parent and child
  append buffers, not parent-resident, and full/near-full/`16-31` free-slot.
- `direct_write_dirty_page_buffer_merge_entry()` writes the child page image
  through the same dirty-page buffer entry writer used by pressure flushes.
- `mylite_storage_rollback_statement()` restores dirty-page undo preimages,
  restores append-buffer undo preimages, flushes append pages before truncation,
  trims append-buffer state, writes the rollback header, truncates to the
  rollback header page count, and flushes the file. Future-current direct writes
  are therefore removable by the existing rollback truncate path because they
  are beyond the statement's durable rollback header page count.
- The latest tail-distance benchmark reports `future-current-header-partial-leaf`
  fallback admissions of `12,036` in the `below-parent-max-by-32-127` band:
  `5,627` with `32-63` free slots, `6,344` with `64-127`, and `65` with
  `128+`. The first two groups record `724` and `1,491` replacement events,
  respectively, while the tiny `128+` group records `388`.

## Design

The tested behavior direct-wrote future-current partial index leaves that
satisfied all existing future-current safety gates plus both of these
admission-time shape predicates:

- the leaf has `32-127` free slots;
- the page id is `32-127` pages below the current max index-leaf page id in
  the parent dirty buffer.

The policy deliberately excludes:

- leaves with `128+` free slots, because even the small `32-127` below-tail
  group was replacement-heavy;
- pages closer than `32` below the parent dirty-buffer tail, because they are
  still near active tail growth;
- pages `128+` below the parent dirty-buffer tail, because that group admitted
  many pages and still recorded `10,793` replacement events;
- append-buffer-resident pages, parent-resident pages, branch pages, pages past
  the parent current header, and any case where the parent dirty buffer is not
  full.

The first implementation added a new direct-write guard outcome. That version
was rejected before benchmarking because it expanded existing static
thread-local guard-outcome counter arrays enough for the embedded MariaDB smoke
benchmark to abort with `Can't initialize timers`. The second implementation
kept the existing `future-current-header-partial-leaf` guard outcome and applied
the below-tail predicate as a contextual direct-write decision so no guard
counter tensor grew.

## Outcome

The behavior is not adopted. The contextual-predicate version completed the
prepared-insert component benchmark, but it regressed the hot step badly:

- prepared insert step: `135.813 us/op` versus the `77.870 us/op` tail-distance
  baseline;
- dirty `index-leaf` merge direct writes: `66,252` versus `53,136`;
- dirty `index-leaf` pressure admissions: `20,815` versus `34,484`;
- branch entry-count fast replacements: unchanged at `115,619`;
- branch entry-count fence fast replacements: unchanged at `13,922`;
- leaf growth fast replacements: `34,346` versus `33,851`.

The pressure reduction is real, but the added direct writes and repeated
publication cost dominate on the VPS smoke profile.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes are committed. `ENGINE=InnoDB` continues to route
through MyLite.

## Single-File And Lifecycle Impact

No new files or durable state are introduced. The rejected behavior would have
kept direct-written future-current pages inside the parent statement's current
header page count and outside append buffers, with rollback still relying on
the existing durable-header truncate path.

## Public API And Binary Impact

No public API changes and no dependencies are committed.

## Tests And Verification

- The local behavior implementation added and passed a focused storage
  self-test that filled the parent dirty buffer, installed a parent dirty leaf
  tail, merged a child future-current leaf `32-127` pages below that tail with
  `32-127` free slots, and asserted direct publication, no pressure flush, no
  fallback parent-rank admission, and rollback truncation removability.
- The local contextual-predicate implementation completed the prepared-insert
  component benchmark. The baseline before the experiment was:
  - prepared insert step: `77.870 us/op`;
  - dirty `index-leaf` merge direct writes: `53,136`;
  - dirty `index-leaf` pressure admissions: `34,484`;
  - branch entry-count fast replacements: `115,619`;
  - branch entry-count fence fast replacements: `13,922`;
  - leaf growth fast replacements: `33,851`.
- The durable docs-only slice is verified with `git diff --check`; no behavior
  code remains in the committed tree.
- A future attempt to adopt a revised behavior still needs the full storage and
  storage-smoke verification loop before commit.

## Acceptance Criteria

- The rejected behavior evidence is recorded with the benchmark result.
- No direct-write behavior change is committed.
- Future work does not expand the guard-outcome enum without moving existing
  high-dimensional test-hook counter tensors off static TLS.

## Risks

- Page-id distance remains a locality heuristic, not B-tree semantic distance.
- A later insert can revisit a direct-written future leaf and force another
  direct write, so replacement evidence must be checked after implementation.
- The parent dirty-buffer tail scan only ran on the already-narrow
  future-current partial-leaf path, but the benchmark still showed that the
  resulting direct writes erased the pressure benefit.
