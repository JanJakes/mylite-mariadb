# Ownerless Index Fill-Sparse Prefilter

## Goal

Keep the newly proven `FIL_PAGE_INDEX` fill-sparse capability without paying a
full-page fill-sparse scan for index pages whose already-built compact sparse
payload has no repeated fill run to exploit.

## Non-Goals

- Do not change the page-log record format or add new record flags.
- Do not change ownerless page-version visibility, snapshot retention,
  checkpoint policy, or native redo/checkpoint semantics.
- Do not disable index fill-sparse for pages that actually contain repeated
  nonzero fill runs.
- Do not claim ownerless concurrency complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines `FIL_PAGE_INDEX` as the
  native uncompressed B-tree index page type at `FIL_PAGE_TYPE` offset 24.
- `packages/libmylite/src/ownerless_page_log.cc` already builds compact or
  varint compact sparse payloads by scanning nonzero page runs before it
  considers fill-sparse encoding.
- The index fill-sparse slice proved byte-exact replay and checkpoint
  retention for fill-sparse `FIL_PAGE_INDEX` records, but the representative
  simple insert production probe still selected fill-sparse only for the SYS
  record. The index bucket stayed around `1779` payload bytes per insert while
  append encode time rose compared with the SYS-only sample.

## Compatibility Impact

There is no SQL, C API, mysqli, WordPress, or wire-protocol behavior change.
Existing page-log records remain readable because the slice only changes when
the existing fill-sparse format is attempted for index pages.

## Design

Add a first-party helper that inspects the already-built compact sparse
payload and returns true only when its raw nonzero data contains at least one
same-byte run long enough for the fill-sparse encoder to represent as a fill
run and the estimated fill-sparse encoding is smaller than the compact payload.
For `FIL_PAGE_INDEX`, `encoded_payload_size_for_page()` should call
`build_fill_sparse_zero_payload()` only when that prefilter is true.

`FIL_PAGE_TYPE_SYS` keeps the direct fill-sparse attempt because the measured
SYS proof page is known to benefit and that path is the source of the current
major payload reduction. Index pages still use fill-sparse when the prefilter
finds a candidate run, preserving the correctness coverage from the previous
slice.

## File Lifecycle

No file lifecycle change. Durable state remains in the existing ownerless
page-version WAL under the MyLite database directory.

## Embedded Lifecycle And API

No public API, open/close, or embedded runtime lifecycle behavior changes.

## Build, Size, And Dependencies

No dependency or build-profile change is introduced. The implementation adds a
small parser over the compact sparse payload already materialized in memory.

## Test Plan

- Extend ownerless primitive coverage to prove:
  - index pages with repeated fill runs still select fill-sparse,
  - index pages without a repeated fill run stay on compact or varint compact
    sparse encoding and do not increment fill-sparse counters,
  - exact replay and snapshot-boundary retention still pass.
- Run focused production ownerless primitive and SQL selectors.
- Run stats-enabled production probe and verify the representative insert path
  still records only the SYS fill-sparse record while avoiding the broader
  index fill-build path.
- Run hook, stress, CI production-build, formatter, and whitespace checks.

## Acceptance Criteria

- Focused primitive and SQL selectors pass.
- The simple insert stats-enabled probe still reports `1.000` fill-sparse
  record per insert, proving ordinary index pages did not select the fill path.
- Append encode time returns close to the SYS-only sample rather than staying
  inflated by no-benefit index fill-sparse attempts.
- Docs identify that user/index payload remains an unsolved performance target.

## Implementation Evidence

The primitive page-log test now keeps the previous `FIL_PAGE_INDEX`
fill-sparse exact-replay and checkpoint-boundary coverage, and adds an index
page whose sparse nonzero bytes contain no fill-sized repeated run. That page
stays on compact sparse encoding, does not increment the fill-sparse counter,
is attributed to the index bucket, and replays byte-for-byte.

The reduced 100-row stats-enabled production probe reported `1.000`
fill-sparse record per ownerless autocommit insert, still the SYS page, with
`1779.030` index payload bytes, `159.340` undo-log bytes, and `72.030` SYS
bytes per insert. Total page-log append was `0.088 ms/insert`, append encode
was `0.059 ms/insert`, and the same run reported ownerless autocommit at
`1364.26 ops/s` versus ordinary autocommit at `2907.78 ops/s`.

This restores the representative append/encode cost close to the SYS-only
fill-sparse baseline while preserving the index fill-sparse correctness path.
The remaining payload is still dominated by user/index pages, so later
performance work needs a stronger user/index page-version representation or
native boundary proof rather than broader fill-run scanning.

## Risks And Open Questions

The prefilter scans compact raw data, so it is not free. It is still bounded by
the already-selected compact payload size, not the full native page size, and
it avoids constructing a second page encoding for index pages without repeated
fill runs. Later user/index payload work likely needs a delta or boundary-aware
representation, not another fill-run heuristic.
