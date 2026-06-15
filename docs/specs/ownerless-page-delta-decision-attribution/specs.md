# Ownerless Page-Delta Decision Attribution

## Problem Statement

The ownerless page-log append attribution slice showed that a 500-row
stats-enabled production sample still spent meaningful time in delta encoding
and standalone encoding after page-publish buffer reuse. The same sample showed
many accepted deltas that were not fast accepted: index exact deltas were
`index_delta_records - index_delta_fast_records`, and undo exact deltas were
`undo_delta_records - undo_delta_fast_records`.

Before changing the fast-path rule, MyLite needs to know whether those fast
misses are caused by the bounded fast payload limit, by the
stored-base standalone-size comparison, by delta-build failure, or by exact
fallback rejecting the delta after comparing against the current standalone
payload.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/include/fil0fil.h` defines the InnoDB page type
  metadata used by the existing page-log eligibility checks for
  `FIL_PAGE_INDEX` and `FIL_PAGE_UNDO_LOG`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  remains the native page-image source. This slice does not change page
  publication, checksum preparation, redo assignment, or native recovery.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  owns the fast delta attempt, standalone fallback encode, exact delta attempt,
  checksum, payload write, and process-local base update.
- `packages/libmylite/src/ownerless_page_log.cc::maybe_encode_page_delta_payload()`
  already has all decision inputs: delta build success, delta payload size,
  the fast payload limit, and the half-standalone-size comparison.
- `packages/libmylite/tests/embedded_performance_probe.c` mirrors the private
  page-log append perf counter order and emits raw plus per-insert summary
  ownerless performance keys.

## Design

Keep the durable WAL format and delta selection rule unchanged. Add
append-only page-log append perf counters for:

- accepted fast delta payload bytes split by index and undo page class;
- accepted exact delta record and payload bytes split by index and undo page
  class;
- fast-path misses caused by the fast payload limit;
- fast-path misses caused by the stored-base standalone-size comparison;
- fast-path and exact-path delta-build failures;
- exact fallback rejections caused by the current standalone-size comparison.

`maybe_encode_page_delta_payload()` now reports an optional decision reason and
the built delta payload size to the append caller. The append caller records a
fast rejection when the fast attempt fails, then records either an exact
accepted delta or exact rejection after the standalone fallback has produced
the current standalone size.

## Compatibility Impact

This slice is instrumentation-only. It does not change SQL behavior, public
`libmylite` API, MariaDB C API behavior, page-log record format, checkpoint
format, native storage files, or unsupported server-surface policy.

## Database Directory And Native Storage Impact

No durable files, directory layout, ownerless page-version WAL records,
checkpoint records, native InnoDB pages, or recovery semantics change. The new
counters are process-local diagnostics exposed through existing internal test
hooks.

## Embedded Lifecycle Impact

No startup, close, process-slot, ownerless lock, recovery, or cleanup lifecycle
behavior changes. The counters reset through the existing append-perf reset
hook and are meaningful only when append perf stats are enabled.

## Public API, Wire Protocol, Binary Size, And Dependencies

No public API, wire-protocol, dependency, or build-profile change. Binary-size
impact is limited to private counter updates and additional performance-probe
output strings.

## Tests And Verification Plan

- Build the production ownerless primitive test, cross-process SQL target, and
  embedded performance probe.
- Run ownerless primitive coverage. The deterministic small index-delta test
  asserts accepted fast payload bytes match the existing delta payload counter.
- Run a reduced stats-enabled production embedded performance probe and verify
  the new detailed and compact summary keys are emitted.
- Run a focused ownerless SQL history-WAL-proof selector to keep the native
  proof path covered.
- Run production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Existing page-log append counter indexes stay stable; new counters are
  append-only after the existing append-attribution tail.
- Fast accepted and exact accepted delta decisions are distinguishable in probe
  output.
- Fast misses and exact rejections are attributed to the decision that rejected
  them.
- Primitive ownerless page-log tests still prove byte-exact delta readback and
  checkpoint behavior.
- Docs identify this slice as evidence for the next optimization, not as a
  new ownerless correctness guarantee.

## Risks And Unresolved Questions

- The counters do not reduce runtime cost by themselves. They identify whether
  the next optimization should adjust the fast limit, the fast acceptance
  heuristic, or a different append subpath.
- The rejection counters are intentionally process-local performance evidence;
  they are not durable state and are not part of the public C API.
