# Ownerless Delta Exact Negative Cache

## Problem

The ownerless page-log append path first tries a fast page-delta decision using
the cached standalone size for the current delta base. When the fast decision
rejects a retained delta because standalone encoding looks smaller, exact
fallback probes the current page's standalone size before choosing between the
retained delta and a standalone record.

That exact fallback is safe but still visible in reduced production append
attribution. Repeated fill-sparse and compact-sparse page shapes can reject the
same class of fast-miss delta again after the base slot is refreshed to the
standalone page. Recomputing the exact standalone-size probe in that case
preserves compression opportunities, but it is not required for correctness:
choosing the standalone page image still replays to the same bytes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` remains the source of the stable
  committed page image that MyLite appends to the ownerless page-version log.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  performs fast delta selection, exact fallback, standalone materialization,
  page-log write, and delta-base refresh.
- `index_delta_base_snapshot()` exposes the transaction-local page-log base
  slot used by the fast path. `note_index_delta_base_after_successful_append()`
  refreshes that slot after standalone records and increments delta-chain
  length after accepted delta records.
- Existing primitive coverage proves that fast-limit misses must still reach
  exact fallback because a retained payload larger than the fast limit can
  still beat the current standalone encoding.

## Design

Record a small per-delta-base counter when exact fallback rejects a retained
fast-miss delta in favor of standalone encoding. When a later append observes
the same page-log base slot with at least one prior exact standalone rejection
and the new fast decision is also `Standalone`, skip the exact standalone-size
probe and materialize a standalone record directly.

The cache is intentionally conservative:

- it applies only after a fast `Standalone` decision, not after `FastLimit`;
- it is tied to the existing page-log device/inode/offset/generation,
  delta flag, space id, page number, and page size base slot;
- it is reset on accepted delta records, ordinary standalone refreshes,
  allocation failure, and log invalidation;
- it changes only compression selection, never record replay bytes or commit
  ordering.

Add append performance counters for skipped exact standalone probes so the
production probe can report whether the cache is active and whether probe
calls fall as expected.

The first reduced stats-enabled production sample after implementation used
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=100`,
`MYLITE_PERF_INSERT_ITERATIONS=120`,
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
`MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1`. It reported one skipped exact
standalone probe in the single-row autocommit insert shape
(`delta_exact_skipped_standalone_records_per_insert=0.008`) and one in the
four-row bulk shape. This confirms the cache is observable but small in that
sample; the larger write-path costs remain page publication,
history/native-support proof, and redo/checkpoint reconciliation.

## Compatibility Impact

No SQL behavior, public C API, PHP API, mysqli behavior, wire-protocol
behavior, storage format, page-log format, or database-directory layout
changes. The cache can only choose a standalone page image where exact fallback
would otherwise have a chance to choose a smaller delta payload.

## Native Storage And Lifecycle Impact

No native InnoDB page state, redo, undo, checkpoint, purge, lock, process-slot,
startup, close, cleanup, or recovery protocol changes. The committed page
image supplied by InnoDB is still written to the ownerless page-version log
when the cache fires.

## Performance Impact

The expected effect is bounded: repeated exact fallback standalone misses skip
the standalone-size probe and the corresponding exact comparison. The slice
does not reduce page-version publication count, native-support proof pages,
history-proof pages, fsync/checkpoint work, or page-log record count. It may
store standalone records in rare cases where a later exact probe would have
found a newly favorable delta; that is a size/performance tradeoff, not a
correctness risk.

## Test Plan

- Add primitive coverage for two consecutive exact standalone rejections on an
  index page shape, proving that the second append skips the standalone-size
  probe and still replays the standalone page image.
- Preserve existing primitive coverage proving fast-limit misses can still use
  exact fallback and that fill-sparse rejection still stores standalone bytes.
- Run `libmylite.ownerless-primitives`.
- Run focused embedded ownerless SQL selectors that exercise page-log append
  and history/native support paths.
- Run a reduced stats-enabled production embedded performance probe and check
  the new skipped-exact counters plus stable replay-shape counters.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- A repeated exact standalone rejection skips the second standalone-size probe.
- Fast-limit misses still reach exact fallback and can reuse the retained
  payload.
- Page-log replay and latest-page lookup return the exact appended page bytes.
- Production probe output exposes skipped exact standalone counters.
- Focused ownerless primitive and embedded SQL coverage passes.

## Risks And Follow-Up

This is a compression-selection optimization, not the larger ownerless write
performance fix. The remaining high-impact gap is still native page
publication, history/native durability proof, and broader redo/checkpoint
reconciliation. Those paths need proof-first slices because previous MTR
release-decision shortcuts broke stress monotonicity and DDL coverage.
