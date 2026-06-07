# Ownerless Active-Pin Reclaim Policy Alignment

## Problem

The branch contains two related active-pin reclaim documents with different
claims. The implemented product path keeps page-version WAL while any live
page-version pin exists, but `ownerless-single-active-pin-reclaim` still
described an enabled single-pin product compaction path. That made the
performance status ambiguous and overstated the current ownerless completion
claim.

## Source Findings

- `packages/libmylite/src/database.cc:reclaim_ownerless_page_log_after_native_checkpoint()`
  returns immediately for live-peer reclaim when `active_pin_count > 0`.
- `docs/specs/ownerless-active-pin-reclaim/specs.md` already records the
  conservative product policy and the reason: native checkpointing while a
  snapshot runtime is live can disturb redo state needed by concurrent
  ownerless startup.
- `docs/specs/ownerless-native-boundary-synthesis/specs.md` records the
  follow-up behavior: synthesized boundary records help active readers and
  post-release cleanup, but do not trigger product close-time compaction while
  the pin remains active.
- `docs/specs/ownerless-cross-process-concurrency/specs.md` and
  `docs/specs/ownerless-single-active-pin-reclaim/specs.md` still had wording
  that implied active-pin product reclaim could run.

## Design

- Update the cross-process concurrency spec so live-peer product reclaim is
  described as zero-active-pin only.
- Reframe `ownerless-single-active-pin-reclaim` as a deferral document for the
  primitive-backed optimization, not an implemented product behavior.
- Keep compatibility status unchanged: active-reader pressure remains partial,
  with WAL retention under live pins and bounded pressure controls.

## Compatibility Impact

No runtime behavior changes. The slice narrows documentation to match existing
code and tests, reducing the risk of claiming a performance optimization that
is intentionally disabled for correctness.

## Test And Verification Plan

- Focused normal SQL: `live-reclaim`.
- Focused hook SQL: `active-pin-reclaim-boundary`.
- Primitive ownerless page-log retention tests.
- `git diff --check`.

## Acceptance Criteria

- Product close-time reclaim is documented as retaining WAL while any live
  page-version pin exists.
- Single-snapshot compaction remains documented only as lower-level primitive
  evidence.
- Verification passes for the tests that prove live-pin retention and
  post-release cleanup.
