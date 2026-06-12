# Ownerless Varint Compact Sparse Page Log

## Problem

The compact-sparse composition attribution slice showed that reduced ownerless
autocommit page-log payload is still split between data-heavy SYS proof pages
and index pages with a meaningful sparse-run metadata component. In the final
100-row production attribution sample, index compact-sparse payload averaged
`2302.280` bytes per insert, split into `1086.220` metadata bytes and
`1216.060` data bytes. The SYS bucket remained data-dominated, so a smaller
run header is not the main proof-page solution, but it can reduce user/index
payload without changing page visibility semantics.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc::encoded_payload_size_for_page()`
  currently chooses compact 16-bit sparse payloads before legacy 32-bit sparse
  payloads and trailing-zero payloads.
- `build_compact_sparse_zero_payload()` writes `uint16_t run_count`, followed
  by per-run `uint16_t offset`, `uint16_t size`, and the run bytes.
- `read_record_page_payload()` and `read_record_page_type()` decode sparse
  payloads directly from the page log, validate monotonically increasing runs,
  and verify the reconstructed page checksum before returning a page image.
- `record_payload_shape_valid()` rejects records with unknown or multiple
  encoding flags, so adding a new encoding requires updating the known-mask and
  sparse read paths together.
- The primitive and performance-probe tests mirror the page-log append stat
  enum and must be updated in the same slice.

## Design

Add a varint compact sparse payload flag for compact sparse pages whose
nonzero runs can be represented more cheaply as varuint16 gaps and run sizes:

```text
uint16_t run_count
repeated run_count times:
  varuint16 zero_gap_from_previous_run_end
  varuint16 run_size
  uint8_t run_bytes[run_size]
```

The encoder will first build the existing 16-bit compact sparse payload, then
derive the varint form from those encoded bytes. It will choose the varint form
only when every gap and run size fits in the same 16-bit range as the compact
payload and the derived payload is smaller than the 16-bit compact payload.
Pages whose varint headers would not improve size keep the current 16-bit
compact encoding, and pages outside compact limits keep the existing legacy
sparse or trailing-zero fallback behavior.

The sparse decoder will treat the new flag as another sparse-zero payload
variant, reconstruct the absolute run offset from the previous run end plus
the decoded gap, preserve the existing overlap/bounds checks, and verify the
full page checksum after reconstruction. `read_record_page_type()` will decode
just enough varint metadata to find the page-type bytes without materializing
the whole page, matching the existing compact and legacy sparse path.

Append statistics will continue to count varint compact records as sparse-zero
and compact-sparse records so existing aggregate probe lines remain useful.
The probe will add a varint-compact subset count and payload byte total so the
byte reduction is visible in raw output and per-insert summaries.

## Scope And Non-Goals

In scope:

- new varint compact sparse page-log record encoding,
- readback and page-type decoding for that encoding,
- primitive coverage for varint encoding, 16-bit compact fallback, and legacy
  32-bit fallback,
- production attribution probe evidence for ownerless autocommit payload
  reduction.

Out of scope:

- changing page checksums or record-header layout,
- changing page-version visibility, checkpoint, or recovery ordering,
- delta encoding against native pages or previous WAL records,
- solving the data-heavy SYS history-proof page representation.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli behavior, or durable MariaDB native file
format changes. This changes MyLite's internal ownerless page-version WAL
encoding for newly appended records only; existing full, trailing-zero, legacy
sparse, and 16-bit compact sparse records remain readable.

## Directory And Lifecycle Impact

No new files or directory-layout changes. The existing
`concurrency/mylite-concurrency.wal` stores a smaller payload for qualifying
new page-version records.

## Native Storage Impact

No native InnoDB page, redo, undo, checkpoint, or recovery semantics change.
The reconstructed page image must still match the recorded full-page checksum.

## Build And Performance Impact

The encoder derives the varint form from the already-built 16-bit compact
payload, avoiding a second scan of the source page. Qualifying tiny-run pages
write fewer WAL bytes and fewer sparse metadata bytes; pages with large gaps or
runs that do not benefit from varint headers pay only a bounded parse of the
compact payload before keeping the current format.

## Test And Verification Plan

- Extend ownerless primitive tests for:
  - varint compact sparse encoding and readback,
  - 16-bit compact fallback when varint headers are not smaller,
  - legacy 32-bit sparse fallback when compact offsets exceed 16-bit limits.
- Extend append stats mirrors and performance-probe output with the varint
  compact subset counters.
- Run focused ownerless primitive coverage.
- Run a reduced production stats-enabled embedded performance probe and record
  compact/varint payload evidence.
- Run focused ownerless SQL selectors for committed-read, commit-race, and
  local write/read sanity.
- Run production build guards, format check, and whitespace check.

## Implementation Evidence

The implementation adds a new internal page-log encoding flag for varint
compact sparse payloads. The encoder still builds the existing 16-bit compact
sparse payload first, derives varuint16 gap/run metadata from those bytes, and
selects the varint form only when the derived payload is strictly smaller. The
decoder reconstructs absolute run offsets from previous-run-end plus decoded
gap, preserves the existing overlap and bounds checks, and verifies the full
page checksum after reconstruction.

Primitive coverage verifies three record families:

- a four-run 128-byte sparse page uses varint compact sparse payload, shrinking
  the deterministic payload from the prior 16-bit `22` bytes to `14` bytes,
  split into `10` metadata bytes and `4` data bytes;
- a 32 KiB page with a large gap and 128-byte run stays on the 16-bit compact
  sparse fallback because the varint form is not smaller;
- a 70 KiB page whose nonzero offset exceeds the 16-bit compact range stays on
  the legacy 32-bit sparse fallback.

The reduced 100-row production stats-enabled performance probe reported:

- total page-log append payload: `607816` bytes (`6078.160` per insert),
- varint compact sparse records: `302` (`3.020` per insert),
- varint compact sparse payload: `607816` bytes (`6078.160` per insert),
- compact sparse metadata: `64220` bytes (`642.200` per insert),
- compact sparse data: `543596` bytes (`5435.960` per insert),
- index compact sparse split: `54511` metadata bytes and `121607` data bytes,
- SYS compact sparse split: `2856` metadata bytes and `412371` data bytes.

Compared with the preceding compact-sparse composition sample, total page-log
payload fell by about `623` bytes per ownerless autocommit insert and compact
sparse metadata fell by about `624` bytes per insert. The remaining SYS payload
is still almost entirely nonzero data, so later performance work must target
the SYS proof representation rather than another sparse metadata encoding.

## Verification Results

The slice was verified with production embedded builds:

- `cmake --build --preset embedded-prod --target mylite_ownerless_primitives_test mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20 MYLITE_PERF_INSERT_ITERATIONS=100 MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test prepared-committed-read`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test commit-race`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test local-write-first-read`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `tools/require-cmake-release-build build/embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod`
- `git diff --check`

## Acceptance Criteria

- Existing 16-bit compact, legacy sparse, trailing-zero, and full-page records
  remain readable.
- Varint compact records reconstruct exactly and pass checksum validation.
- Varint compact records are chosen only when they are strictly smaller than the
  16-bit compact payload.
- Aggregate compact-sparse stats still include all compact sparse variants,
  while varint compact subset stats identify the new encoding's contribution.
- The performance probe shows whether index/page-log payload bytes decrease in
  the reduced ownerless autocommit sample.

## Risks And Unresolved Questions

- This does not address the data-dominated SYS history-proof payload.
- The reduced simple-insert sample may overrepresent tiny-run index pages
  compared with broader DDL, BLOB, or compressed-page workloads.
- Future delta/proof formats must keep versioning clear so mixed WAL records
  remain readable during recovery.
