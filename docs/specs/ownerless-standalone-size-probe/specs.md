# Ownerless Standalone Size Probe

## Problem Statement

The ownerless delta fast-miss reuse slice removed duplicate delta construction
for exact fallback, but exact fallback still materializes a standalone payload
before it can decide whether the retained delta wins against the current page
image. In the reduced 200-row production sample, all `84` exact accepted
deltas reused a fast-miss payload, so those records built a standalone payload
only to discard it.

The next bounded optimization is to compute the exact current standalone
payload size without materializing the standalone bytes. When that exact size
proves the retained delta wins, the append path can skip standalone payload
construction entirely.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  remains the native page-image source. This slice does not alter page
  publication, checksums, redo assignment, or native recovery.
- `packages/libmylite/src/ownerless_page_log.cc::encoded_payload_size_for_page()`
  selects among full, trailing-zero, sparse-zero, compact sparse, varint
  compact sparse, and fill-sparse standalone payloads. With an output vector,
  it materializes candidate payload bytes because the caller may need to write
  the standalone record.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  currently materializes the standalone payload before exact fallback can
  compare the retained fast-miss delta against the current standalone size.
- The existing exact decision requires only the selected standalone payload
  size. The standalone bytes are needed only when exact fallback does not
  select the delta.

## Design

Add a size-only standalone payload probe that mirrors the existing standalone
selection rules and returns the exact selected standalone payload size without
writing bytes into a vector. The probe computes sizes for:

- trailing-zero payloads;
- legacy sparse-zero payloads;
- compact sparse and varint compact sparse payloads;
- fill-sparse payloads, including the existing `FIL_PAGE_INDEX` and
  `FIL_PAGE_SYS` fill-candidate rule;
- full-page fallback.

In `append_record_at_locked()`, when a fast attempt has already built and
retained a rejected delta payload, run the size-only probe before standalone
materialization. If the retained delta is still less than half of the exact
current standalone size, swap the retained delta into the record payload,
record an exact accepted delta, and skip standalone materialization. If the
retained delta does not win, fall back to the existing standalone
materialization and write the same standalone record as before.

Add private append-perf counters for:

- standalone size-probe calls and nanoseconds;
- standalone materialization skips and skipped standalone bytes.

## Compatibility Impact

No SQL behavior, public `libmylite` API, MariaDB C API behavior, wire-protocol
behavior, page-log record format, checkpoint format, or native storage
semantics change. The optimization changes only whether exact fallback builds
standalone bytes before selecting an already-built delta.

## Database Directory And Native Storage Impact

No durable files, directory layout, page-version WAL records, checkpoint
records, native InnoDB pages, or recovery behavior change. Records selected by
the size probe use the same delta payload and checksum validation path as
records selected after standalone materialization.

## Embedded Lifecycle Impact

No startup, close, process-slot, lock, cleanup, or recovery lifecycle behavior
changes. The probe uses only transient stack/local counters and the already
stable page image.

## Public API, Wire Protocol, Binary Size, And Dependencies

No public API, wire-protocol, dependency, or build-profile change. Binary-size
impact is limited to first-party size-calculation helpers and private
diagnostic output keys.

## Tests And Verification Plan

- Extend ownerless primitive coverage so the deterministic fast-limit miss
  asserts exact fallback skipped standalone materialization and still
  reconstructs the latest page byte-for-byte.
- Build the production ownerless primitive test, cross-process SQL target, and
  embedded performance probe.
- Run ownerless primitive coverage and the focused ownerless
  history-WAL-proof selector.
- Run a reduced stats-enabled production embedded performance probe and
  verify size-probe and standalone-skip counters.
- Run hook/product ownerless subset, deterministic stress-trace selector,
  production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Exact fallback skips standalone materialization only when the exact
  size-only standalone probe proves the retained delta wins.
- If the retained delta does not win, the append path still materializes and
  writes the same standalone payload selected by the existing encoder.
- Existing page-log record encoding, replay, latest-page lookup, and checkpoint
  behavior remain unchanged.
- Performance probe output exposes size-probe cost and skipped standalone
  materialization counts.

## Implementation Evidence

- `test_page_log_reuses_fast_miss_delta_payload_for_exact_fallback()` now
  verifies the size probe, one skipped standalone materialization, the skipped
  standalone byte count for a known full-page standalone candidate, and
  byte-exact latest-page reconstruction.
- A reduced 200-row stats-enabled production probe reported
  `84` skipped standalone materializations, `102687` skipped standalone bytes,
  `0.420` skipped materializations per insert,
  `0.027` standalone materialization ms/insert, and `0.011` standalone
  size-probe ms/insert. The preceding same-shape fast-miss reuse sample
  reported `0.035` standalone materialization ms/insert before the size probe
  separated and skipped exact-accepted retained-delta materialization.
- Focused ownerless primitive, history-WAL-proof, hook/product, deterministic
  stress-trace, production-build, format, and whitespace checks passed. The
  combined hook subset timed out once in `embedded-ownerless-trx-hooks`; an
  immediate isolated rerun passed in `1.63` seconds, and the remaining
  hook/product selectors passed separately.

## Risks And Unresolved Questions

- The size-only probe must remain semantically aligned with
  `encoded_payload_size_for_page()`. Future standalone encoding changes must
  update both paths or replace them with a shared selector abstraction.
- This optimization reduces exact-accepted retained-delta fallback cost only.
  Standalone records that are actually written still require materialization,
  and native history-proof publication plus redo/checkpoint reconciliation
  remain separate work.
