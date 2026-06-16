# Ownerless Checkpoint Legacy Write Elision

## Problem

Ownerless raw-latest and page-visible checkpoint publication still writes two
representations of the latest/visible LSN pair on every advancing checkpoint
update:

- the current checksummed generation record, and
- the legacy fixed payload at the front of `mylite-concurrency.ckpt`.

After the LSN-record format is initialized, readers prefer the highest valid
generation record. They fall back to the legacy payload only while both record
slots are empty. Rewriting the legacy payload after a valid record already
exists therefore adds checkpoint hot-path write work without contributing to
current recovery semantics.

## Source Findings

- `packages/libmylite/src/database.cc::read_concurrency_checkpoint_lsn()`
  calls `read_concurrency_checkpoint_lsn_records()` first and returns the
  highest valid generation record when present.
- `read_concurrency_checkpoint_lsn_records()` returns failure, not legacy
  fallback, if it sees non-empty record slots but no valid checksum-protected
  record. This preserves fail-closed torn-record behavior.
- `write_concurrency_checkpoint_lsn_locked()` already reads the current record
  while holding the checkpoint byte-range lock so it can choose the next
  generation and detect same-pair no-op updates.
- The production attribution probe reports four checkpoint update calls per
  ownerless autocommit insert in the current local write path.

## Design

Keep writing the legacy latest/visible payload only when no valid generation
record currently exists. Once `has_current_record` is true, an advancing
checkpoint update writes the next checksum-protected generation record and
skips the legacy payload write.

The existing durable sync rules remain unchanged. Durable updates still sync
the checkpoint file after the record write, and the process-local durable sync
anchor is still updated only after a successful sync. Same-pair no-op elision
continues to run before this path.

Add a database perf counter and probe summary key for
`checkpoint_update_legacy_write_elided` so CI can verify the hot path has moved
off the legacy payload write after initialization.

## Compatibility Impact

No SQL behavior, public API, native storage format, page-version WAL, or
checkpoint record format changes. The legacy payload remains initialized for
empty-record fallback and older metadata initialization paths. Current readers
continue to use generation records when present and fail closed on torn
non-empty record slots without falling back to stale legacy bytes.

## Performance Impact

The ownerless checkpoint update path avoids one 16-byte positioned write per
advancing checkpoint update after record initialization. In the current local
stats-enabled 1000-row probe, ownerless autocommit reported
`4.000` legacy checkpoint writes elided per insert and checkpoint write time at
`0.012 ms/insert` versus an earlier local `0.019 ms/insert` sample. The
throughput sample was noisy, so this is evidence for reduced checkpoint write
work, not a claim that the broader write-path gap is closed.

## Test Plan

- Build the production embedded performance probe and ownerless SQL harness.
- Add a focused ownerless SQL case that advances a checksum-protected
  checkpoint record through the internal test hook, verifies the legacy payload
  stayed at the pre-existing pair, and verifies the new perf counter.
- Run adjacent checkpoint LSN generation/no-op/torn-record cases.
- Run a reduced stats-enabled production performance probe and verify
  `checkpoint_update_legacy_write_elided_per_insert`.
- Run production-build guards, format check, and whitespace checks.

## Acceptance Criteria

- Checkpoint updates still initialize the legacy payload before any valid LSN
  generation record exists.
- Advancing updates after record initialization skip the legacy payload write
  while writing a valid next generation record.
- Existing no-op generation elision and torn-record recovery coverage still
  pass.
- Probe output exposes the legacy-write elision count.

## Risks And Non-Goals

- This does not batch checkpoint updates, reduce checkpoint lock acquisitions,
  or change durable sync ordering.
- This does not reduce the three page-version records currently required by
  the ownerless autocommit history/native-support proof.
- Older binaries that only understand the legacy payload are not a supported
  downgrade path for an actively updated ownerless checkpoint file.
