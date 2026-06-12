# Ownerless Fill-Sparse Page Log

## Problem Statement

Ownerless write-performance profiling after history-proof identity attribution
showed that simple InnoDB autocommit `INSERT ... VALUES` still writes about
`6077` ownerless page-log payload bytes per insert. The remaining native
support payload is dominated by rollback-segment SYS proof pages: the reduced
stats-enabled production sample reported about `4152` SYS payload bytes per
insert even after direct varint compact-sparse page-log encoding removed most
run-metadata overhead.

Manual retained-WAL inspection with an active ownerless reader showed why the
SYS proof pages remain expensive: the page images are sparse relative to zero,
but large portions of the nonzero payload are repeated `0xff` fill bytes. The
existing sparse formats reconstruct from a zero-filled page and copy every
nonzero byte literally, so those fill regions are still logged as raw payload.

This slice adds a bounded page-log payload format that preserves exact page
image reconstruction while encoding repeated nonzero fill runs compactly.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB InnoDB mini-transaction commit remains the native page-image
  boundary. `mariadb/storage/innobase/mtr/mtr0mtr.cc` writes page LSN state and
  dirty-page metadata during MTR commit; this slice does not move that
  boundary.
- InnoDB page classes are identified from the native FIL page type at offset
  24. MyLite page-log attribution and checkpoint boundary decisions use that
  native page type in `packages/libmylite/src/ownerless_page_log.cc`.
- MyLite page-version publication is first-party ownerless infrastructure.
  `packages/libmylite/src/ownerless_page_log.cc` owns page-log record flags,
  append encoding, page reconstruction, page-type peeking for checkpoint
  policy, and append performance counters.
- Existing page-log formats already include full pages, trailing-zero pages,
  legacy sparse-zero payloads, compact sparse-zero payloads, and varint compact
  sparse-zero payloads. Those formats remain readable.

## Design

Add a new page-record flag, `k_record_flag_fill_sparse_zero_payload`, for a
zero-filled reconstruction format with explicit nonzero runs:

```text
u16 run_count
repeat run_count times:
  varuint16 zero_gap_from_previous_run_end
  varuint16 run_size
  u8 kind
  if kind == raw:
    run_size raw bytes
  if kind == fill:
    one repeated fill byte
```

The encoder scans nonzero regions only for native `FIL_PAGE_TYPE_SYS` pages,
the measured history-proof hot class. Runs of the same nonzero byte with length
at least eight bytes are encoded as fill runs; other nonzero bytes are grouped
as raw runs until the next fill candidate. The encoded payload is selected only
when it is smaller than the existing compact/varint sparse payload for the
same SYS page. If it is not smaller, the existing format choice is preserved.
Application index and undo pages keep the existing sparse choices so they do
not pay an extra fill-run scan.

The decoder still reconstructs the full page into a zero-filled buffer and
verifies the stored page checksum against the complete reconstructed image.
The checkpoint page-type fast reader also understands fill runs so native
support pages such as SYS and undo pages keep the existing boundary policy.

## Scope And Non-Goals

- Scope: first-party ownerless page-log payload encoding, decoding, page-type
  peeking, append performance counters, primitive tests, and performance probe
  output.
- Non-goal: skipping rollback-segment or undo history proof pages.
- Non-goal: changing page-version visibility, active-reader retention,
  checkpoint eligibility, native redo/checkpoint recovery, DDL file lifecycle,
  or InnoDB MTR boundaries.
- Non-goal: declaring ownerless concurrency complete.

## Compatibility Impact

There is no SQL, C API, mysqli, or WordPress compatibility change. Existing
page-log records remain readable. New fill-sparse records require a MyLite
reader that understands the new record flag; the ownerless page-version WAL is
internal MyLite directory state and is not a stable external interchange
format.

## Directory And Lifecycle Impact

Durable state remains inside `concurrency/mylite-concurrency.wal` under the
MyLite database directory. No new files, directories, environment variables, or
runtime services are introduced.

## Native Storage Impact

The encoded payload still represents exact native InnoDB page bytes. Recovery
and page refresh paths reconstruct the same full page image before checksum
validation and replay. The slice therefore reduces WAL byte volume without
weakening native storage semantics.

## Binary Size And Dependencies

No dependency is added. The implementation adds a small amount of first-party
codec and metric code to `libmylite`; it does not change the bundled MariaDB
embedded profile.

## Test And Verification Plan

- Add primitive coverage proving fill-sparse records:
  - are selected for a SYS-shaped page with repeated nonzero fill regions,
  - produce expected payload, metadata, raw-data, and fill-byte counters,
  - reconstruct the exact page through both latest-page and direct-record
    reads,
  - remain visible to checkpoint page-type classification as a native support
    page that does not require a snapshot boundary.
- Keep existing compact and legacy sparse fallback tests passing with fixtures
  that do not accidentally match the new fill-run shape.
- Extend embedded performance probe counters and per-insert/per-bulk summaries
  so production timing logs show fill-sparse records, payload bytes, metadata
  bytes, raw-data bytes, and fill bytes.
- Run focused ownerless primitive, embedded ownerless SQL, hook, stress, format,
  and production build-guard checks.
- Record reduced stats-enabled and stats-off production probe results in
  `docs/COMPATIBILITY.md` and the ownerless concurrency spec.

## Acceptance Criteria

- Existing page-log formats and tests continue to pass.
- Fill-sparse page-log records reconstruct exact page bytes and preserve page
  type peeking for checkpoint policy.
- Stats-enabled production probes show the new fill-sparse counters and reduce
  SYS/history-proof payload byte volume for the measured ownerless autocommit
  workload.
- The final diff stays confined to first-party MyLite page-log, tests, probes,
  and documentation.

## Risks And Follow-Up

The format reduces payload bytes but does not remove the need to publish
history-proof pages. If ownerless autocommit remains materially slower than
ordinary mode after this slice, the next high-impact work is broader native
redo/checkpoint reconciliation or a stronger proof that some native-support
page publication can be skipped, not another purely syntactic sparse encoding.
