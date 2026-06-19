# Ownerless Standalone Probe Single Pass

## Problem

The ownerless page-log exact fallback path uses a size-only standalone payload
probe before deciding whether a retained fast-miss delta still beats the
current page image's standalone encoding. For `FIL_PAGE_INDEX` and
`FIL_PAGE_TYPE_SYS` pages, the probe first scanned the page to compute compact
sparse size and then scanned the same page again to see whether fill-sparse
would be smaller.

Recent production attribution still shows native page publication and
history-proof volume as the larger write-path target, but the standalone-size
probe remains visible in reduced ownerless autocommit and four-row bulk insert
samples. This slice removes the duplicate scan without changing the delta
acceptance rule.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` still provides the stable
  committed page image that MyLite appends to the ownerless page-version log.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  exact fallback may retain a fast-miss delta payload, compute the current
  standalone payload size through `encoded_payload_size_for_page_probe()`, and
  select the delta only when the same "less than half of standalone" rule
  remains true.
- `encoded_payload_size_for_page()` can select full, trailing-zero,
  sparse-zero, compact sparse, varint compact sparse, or fill-sparse payloads.
  The size-only probe must stay aligned with those selection rules because an
  overestimated standalone size could select a delta that the exact rule should
  reject.

## Design

Add a private `compact_or_fill_sparse_zero_payload_size_for_page()` helper for
the size-only probe. For fill-sparse candidate page types, it computes compact
sparse, varint compact sparse, fill-sparse, and trailing-size evidence in one
page pass, then returns the same selected standalone size the old two-pass
probe returned.

The helper is used only by `encoded_payload_size_for_page_probe()` for
`FIL_PAGE_INDEX` and `FIL_PAGE_TYPE_SYS` pages. Non-candidate page types still
use the existing compact sparse size helper. If the fill-sparse accounting
becomes invalid or not smaller, the helper falls back to the compact/varint
choice exactly as the previous separate fill probe did.

This preserves:

- page-log record formats and flags;
- exact delta acceptance/rejection rules;
- checkpoint rewrite and replay behavior;
- sparse/fill payload byte selection;
- native page publication and history-proof requirements.

## Compatibility Impact

No SQL behavior, public C API, PHP API, mysqli behavior, wire-protocol
behavior, storage format, page-log format, or database-directory layout
changes. The change only reduces first-party CPU work inside a private
size-only decision helper.

## Native Storage And Lifecycle Impact

No native InnoDB pages, redo, undo, checkpoints, purge behavior, startup,
close, process-slot, lock, cleanup, or recovery lifecycle behavior changes.
The page image is the same stable committed image already passed to the append
hook.

## Performance Impact

The expected effect is a small reduction in exact-fallback standalone-size
probe CPU for index and explicitly hinted SYS/history pages. It does not
reduce page-version publication count, native-support proof pages, page-log
append count, or synchronous publish hook work.

Local reduced production attribution on 2026-06-19 preserved the same
page-log shape. The first post-slice sample reduced measured size-probe time
for the one-row path, while repeated bulk samples were noisy enough that they
should not be treated as a reliable speedup claim:

- one-row ownerless autocommit: standalone-size probe calls stayed `29`,
  payload bytes stayed `542189`, index fast/exact deltas stayed `468`/`13`,
  and probe time moved from `0.631 ms` to `0.307 ms`;
- four-row bulk ownerless autocommit: standalone-size probe calls stayed
  `175`, payload bytes stayed `299051`, index fast/exact deltas stayed
  `69`/`47`; post-slice probe samples ranged from `2.679 ms` to `7.215 ms`
  against the pre-slice `3.192 ms` sample.

A stats-off sanity probe with 2000 inserts reported ownerless autocommit at
`1758.77 ops/s` versus ordinary `3908.47 ops/s` (`0.4500x`) and ownerless bulk
rows at `5714.87 rows/s` versus ordinary `13751.16 rows/s` (`0.4156x`). Treat
these as local sanity numbers; CI production runners remain the comparable
trend signal.

## Test Plan

- Add primitive coverage that forces exact fallback to reject a retained delta
  because the current page's fill-sparse standalone encoding is smaller than
  the compact sparse estimate.
- Run `libmylite.ownerless-primitives`.
- Run focused ownerless SQL selectors for history WAL proof, native-support
  page WAL elision, visible-fast multi-row inserts, and uncommitted peer
  visibility.
- Run a reduced stats-enabled production embedded performance probe and confirm
  page-log append counts, payload bytes, and delta counts remain stable.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- Exact fallback still rejects a retained delta when fill-sparse standalone
  size is smaller than the delta acceptance threshold.
- The size-only probe preserves compact, varint compact, fill-sparse, and
  trailing-zero selection rules.
- Primitive and focused SQL coverage pass.
- Reduced production attribution shows stable payload/count counters and no
  regression in exact delta selection.

## Risks And Follow-Up

The size-only probe remains a parallel implementation of the standalone
encoding selector. Future standalone payload format changes must update both
the materializing encoder and this size-only helper or replace them with a
shared selector abstraction.

The larger ownerless performance gaps remain native history-proof publication
volume, page-version publish hook work, redo/checkpoint reconciliation, and
broader recovery coverage.
