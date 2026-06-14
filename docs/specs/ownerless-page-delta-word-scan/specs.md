# Ownerless Page Delta Word Scan

## Goal

Reduce ownerless page-log delta encoding CPU without changing the page-version
WAL format. Repeated `FIL_PAGE_INDEX` and `FIL_PAGE_UNDO_LOG` records are
mostly identical to their standalone base page; the delta encoder spends time
walking equal bytes before each changed run.

## Design

`build_index_delta_payload()` now skips equal spans in `uint64_t` sized chunks
before falling back to byte scanning. The fallback byte loop still finds the
exact first changed byte, so run boundaries, payload bytes, checksums, and
decode behavior are unchanged.

This is an encoder-only optimization:

- no WAL header or record format changes;
- no page-index format changes;
- no checkpoint/replay semantics changes;
- no cross-process shared-memory layout changes.

## Compatibility Impact

No SQL, C API, or durable-format behavior changes. Readers still decode the
same standalone and delta payloads, and checkpoints still rewrite retained
delta records to standalone records when required.

## Test Strategy

- Build production embedded targets that cover primitive page-log behavior and
  the performance probe.
- Run `mylite_ownerless_primitives_test`, which covers index and undo delta
  encoding, direct reads, latest reads, and checkpoint rewrite behavior.
- Run focused ownerless SQL coverage that exercises page-version publish,
  single-owner history WAL proof, and live reclaim.
- Run the production performance probe with page-publish stats enabled and
  compare delta record counts, payload sizes, and
  `page_log_append_encode_ms` against the pre-slice attribution.

## Evidence

Pre-slice production attribution from the same 500-insert probe shape reported:

- `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=1506`
- `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=29.073`
  to `30.197` across local runs;
- `mylite_perf_summary_ownerless_autocommit_page_log_encode_ms_per_insert=0.058`
  to `0.060`;
- index delta records around `481` and undo delta records around `376`.

Post-slice production attribution reported:

- `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=1506`
- `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=26.987`
- `mylite_perf_summary_ownerless_autocommit_page_log_encode_ms_per_insert=0.054`
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_records=481`
- `mylite_perf_ownerless_insert_autocommit_page_log_append_undo_delta_records=376`

Payload sizes remained in the same range, which is expected because the slice
changes only how the encoder finds equal spans before emitting the existing
delta format.

## Acceptance Criteria

- Primitive delta page-log tests pass.
- Focused ownerless page-version SQL selectors pass.
- Production performance attribution still reports the same page-version and
  delta record counts, with no WAL format changes.
- `page_log_append_encode_ms` does not regress from the pre-slice production
  attribution.
