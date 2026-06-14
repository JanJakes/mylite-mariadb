# Ownerless Page-Log Append Attribution

## Problem Statement

After ownerless page-publish buffer reuse, a production stats-enabled sample no
longer shows meaningful publish allocation/free time. The remaining ownerless
autocommit profile still spends time in page-log append, but the existing
append total includes several subphases that are not separately attributed:
delta-base lookup, fast and exact delta encoding, standalone encoding,
stats-only payload/page-type attribution, and delta-base table updates.

Without that split, follow-up performance work can mistake stats-only
bookkeeping for production runtime cost or choose the wrong WAL-format target.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  remains the InnoDB source of page images. The previous slice removed
  per-publish scratch allocation/free churn without changing page images.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  owns append-time payload selection. It currently wraps the whole payload
  selection block in `PAGE_LOG_APPEND_PERF_ENCODE_NS`, then records checksum,
  payload write, and record-header write timings.
- Inside that encode block, `index_delta_base_snapshot_for_page()`,
  `maybe_encode_page_delta_payload()`, and `encoded_payload_size_for_page()`
  are not timed separately.
- After encoding, `record_append_payload_encoding_stats()`,
  `record_append_page_type_stats()`, and
  `note_index_delta_base_after_successful_append()` run before the append
  returns. These are either stats-only attribution or process-local delta-base
  maintenance, but their time is currently visible only as part of the append
  total.
- The embedded performance probe mirrors the page-log append counter enum and
  emits detailed and compact ownerless autocommit page-log timing keys.

## Design

Add append-only page-log append perf counters for:

- delta-base snapshot lookup,
- delta payload encoding,
- standalone payload encoding,
- payload encoding stats bookkeeping,
- page-type stats bookkeeping,
- delta-base update after a successful append.

Keep the existing `PAGE_LOG_APPEND_PERF_ENCODE_NS` total so older comparisons
continue to work. The new counters are a refinement, not a replacement.

Emit the counters in the embedded performance probe's detailed page-log append
block and compact ownerless autocommit summary.

## Compatibility Impact

This slice is instrumentation-only. It does not change SQL behavior, public
`libmylite` API, page-log record format, checkpoint/replay behavior, native
storage behavior, or unsupported-surface policy.

## Database Directory And Native Storage Impact

No durable files, directory layout, page-version records, native page images,
or checkpoint records change. The new counters are process-local diagnostics.

## Embedded Lifecycle Impact

No startup, close, process-slot, lock, or recovery lifecycle behavior changes.
The counters reset through the existing page-log append perf reset hook.

## Public API, Wire Protocol, Binary Size, And Dependencies

No public API, wire-protocol, dependency, or build-profile change. Binary-size
impact is limited to a few first-party counter updates and probe output keys.

## Tests And Verification Plan

- Build the embedded performance probe and ownerless primitive test under the
  production preset.
- Run ownerless primitive coverage to confirm page-log record encoding and
  replay behavior remain unchanged.
- Run a reduced stats-enabled embedded performance probe and verify the new
  detailed and summary keys are emitted.
- Run production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Existing page-log append counters remain stable and append-only enum
  expansion keeps previous counter positions.
- Stats-enabled probes expose the new subphase timings.
- Page-log primitive tests still pass.
- Docs identify this as attribution for the next optimization, not a WAL
  format or correctness change.

## Risks And Unresolved Questions

- The new counters do not reduce runtime cost by themselves. They are intended
  to distinguish stats-only attribution overhead from production append work
  before the next WAL-format or redo/checkpoint optimization.
- Timing scopes add tiny overhead only when append perf stats are enabled,
  matching the existing counter model.
