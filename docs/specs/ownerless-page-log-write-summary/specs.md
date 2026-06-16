# Ownerless Page-Log Write Summary

## Problem

The ownerless performance probe already emits detailed raw page-log append
counters, but the compact CI summary only exposed total append time, encode
subphases, payload composition, and record counts. Recent local samples showed
the remaining page-log cost can move between encoding, checksum, payload write,
and record-header write buckets. Without those write subphases in the summary,
CI logs do not show whether the next useful WAL slice should reduce record
count, encoding CPU, checksum CPU, or positioned-write overhead.

## Source Findings

- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  already measures append lock, header validation, body/fstat setup, payload
  encoding, checksum, payload write, record-header write, and delta-base note
  time.
- The existing write order is payload first and record header second, so a
  crash cannot expose a valid record header for a missing payload. This slice
  does not change that ordering or record format.
- The stats-enabled production probe already prints the raw detailed keys, but
  the compact `mylite_perf_summary_*` rows did not include the write and
  checksum subphases.

## Design

Promote existing page-log append counters into per-insert ownerless autocommit
summary rows:

- append lock time,
- fstat/header/body setup time,
- checksum time,
- payload write time,
- record-header write time.

No new counters are added and no hot-path instrumentation changes. The summary
only prints values already collected by the stats-enabled probe.

## Compatibility Impact

Diagnostics-only. No SQL behavior, public API, page-version WAL format, WAL
write ordering, checkpoint semantics, native storage behavior, or recovery
logic changes.

## Performance Impact

Stats-off production probes and normal runtime paths are unchanged. The
stats-enabled attribution probe prints a few additional summary rows after the
measured phase completes.

## Test Plan

- Build the production embedded performance probe.
- Run a reduced stats-enabled ownerless performance probe and verify the new
  summary keys are emitted.
- Run production-build guards, format check, and whitespace checks.

## Acceptance Criteria

- CI-facing summaries expose page-log write/checksum subphase costs needed to
  choose the next WAL optimization.
- Existing raw detailed keys and existing summary keys remain present.
- No page-log record format or write-order behavior changes.

## Risks And Non-Goals

- This slice does not reduce runtime cost by itself.
- Combining payload and header writes is intentionally out of scope because it
  would need a separate crash-ordering proof.
