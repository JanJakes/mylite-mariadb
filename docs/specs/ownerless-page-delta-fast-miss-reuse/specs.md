# Ownerless Page-Delta Fast-Miss Reuse

## Problem Statement

The ownerless page-delta decision attribution slice showed that a reduced
200-row production probe had `0.460` fast delta misses per insert, no
delta-build failures, and `0.420` exact accepted deltas per insert. Most fast
misses therefore already paid to build a delta payload, fell back to standalone
encoding, then rebuilt the same delta payload for the exact decision.

That duplicate delta encode work is unnecessary. The exact fallback can reuse
the fast-miss payload after standalone encoding as long as it still performs
the exact current-standalone comparison before selecting the delta.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  remains the native page-image source and is not changed by this slice.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  attempts a fast page delta before standalone encoding, then attempts an
  exact page delta after `encoded_payload_size_for_page()` computes the current
  standalone payload size.
- `packages/libmylite/src/ownerless_page_log.cc::maybe_encode_page_delta_payload()`
  builds the delta payload before applying either the fast limit or the
  standalone-size comparison. Before this slice, rejected fast payloads were
  cleared even when exact fallback immediately needed the same bytes.
- `packages/libmylite/tests/embedded_performance_probe.c` now exposes fast
  miss and exact acceptance counters, making duplicate exact delta encoding
  visible indirectly through `delta_encode_ms`.

## Design

Keep the delta selection rule unchanged:

- fast acceptance still requires the 1024-byte fast limit and the stored-base
  standalone-size comparison;
- exact acceptance still requires the current standalone payload size computed
  by `encoded_payload_size_for_page()`;
- delta records remain non-chained and reference the same durable standalone
  base record.

When the fast attempt builds a delta but rejects it because of the fast limit
or stored-base standalone comparison, retain that rejected payload in a
thread-local scratch vector owned by `append_record_at_locked()`. After
standalone encoding, exact fallback first evaluates the retained payload
against the current standalone payload size. If it wins, swap it into the
record payload and count an exact accepted delta. If it does not win, count the
same exact standalone rejection as before. If the fast attempt failed before
building a payload, exact fallback reports the same build failure rather than
rebuilding.

The retained payload is process-local scratch state, is cleared before each
append attempt, and is never durable.

Add private append-perf counters for exact fallback records and bytes that
reused a fast-miss payload. These counters are append-only after the existing
decision-attribution counter tail.

## Compatibility Impact

No SQL behavior, public C API, MariaDB C API behavior, wire protocol behavior,
page-log record format, checkpoint format, or native storage semantics change.
The optimization changes only how the existing exact decision obtains a delta
payload candidate.

## Database Directory And Native Storage Impact

No durable file, directory layout, page-version WAL record, checkpoint record,
native InnoDB page, or recovery behavior changes. The exact fallback still
writes the same payload shape it would have written after rebuilding the delta.

## Embedded Lifecycle Impact

No startup, close, process-slot, ownerless lock, recovery, or cleanup lifecycle
behavior changes. The retained fast-miss payload is thread-local transient
memory and is cleared inside the append path.

## Public API, Wire Protocol, Binary Size, And Dependencies

No public API, wire-protocol, dependency, or build-profile change. Binary-size
impact is limited to first-party helper code in the page-log append path.

## Tests And Verification Plan

- Build the production ownerless primitive test, cross-process SQL target, and
  embedded performance probe.
- Run ownerless primitive coverage to keep byte-exact delta readback,
  checkpoint rewrite, accepted fast payload counting, and exact fallback reuse
  counting covered.
- Run the focused ownerless history-WAL-proof SQL selector.
- Run a reduced stats-enabled production embedded performance probe and
  compare fast-miss, exact-acceptance, and `delta_encode_ms` evidence.
- Run hook/product ownerless subset, deterministic stress-trace selector,
  production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Exact fallback does not rebuild a delta payload that the fast attempt already
  built and rejected only because of the fast limit or stored-base standalone
  comparison.
- Exact fallback still compares the retained payload against the current
  standalone payload before selecting it.
- Primitive coverage proves a fast-limit miss can be exact-accepted through the
  retained payload and still reconstruct the latest page byte-for-byte.
- Existing page-log record encoding, replay, latest-page lookup, and checkpoint
  behavior remain unchanged.
- Performance probe counters continue to expose fast misses and exact accepted
  deltas for follow-up tuning.

## Implementation Evidence

- `test_page_log_reuses_fast_miss_delta_payload_for_exact_fallback()` forces a
  fast-limit rejection, exact-accepts the retained payload, verifies the reuse
  counter, and reconstructs the latest page byte-for-byte.
- A reduced 200-row stats-enabled production probe reported
  `84` exact fallback records reusing fast-miss payloads, `0` delta-build
  failures, and
  `mylite_perf_summary_ownerless_autocommit_page_log_delta_encode_ms_per_insert=0.006`.
  The preceding same-shape attribution sample before reuse reported
  `0.008` delta encode ms/insert.

## Risks And Unresolved Questions

- This reduces duplicated delta encode CPU only when a fast miss built a
  candidate payload. It does not reduce standalone encoding, checksum, payload
  writes, native history-proof publication, or redo/checkpoint reconciliation.
- If future delta encoders use mutable external state, the retained payload
  rule must continue to ensure the candidate was built from the same stable
  page image and base snapshot used by exact fallback.
