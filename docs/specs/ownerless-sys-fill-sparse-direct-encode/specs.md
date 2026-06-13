# Ownerless SYS Fill-Sparse Direct Encode

## Goal

Avoid materializing a discarded compact sparse payload for
`FIL_PAGE_TYPE_SYS` pages when the existing fill-sparse representation is
known to be smaller. Keep the page-log record format, replay semantics, and
checkpoint retention rules unchanged.

## Non-Goals

- Do not add a new page-log record flag or change durable WAL decoding.
- Do not change index delta selection, native-support proof requirements, or
  page-version visibility.
- Do not claim the ownerless write path is performance-complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The representative ownerless insert path publishes one `FIL_PAGE_TYPE_SYS`
  proof page per row and that page already selects the fill-sparse WAL format.
- Before this slice, `encoded_payload_size_for_page()` built the compact or
  varint compact sparse payload first, then built a fill-sparse payload and
  discarded the compact payload when fill-sparse won. That preserved bytes but
  paid unnecessary allocation/copy work for the hot SYS proof page.

## Compatibility Impact

There is no SQL, C API, PHP, mysqli, or WordPress behavior change. Durable
page-log records remain byte-compatible because the slice only changes how an
existing fill-sparse record is selected and built in memory.

## Design

Add a shared fill-sparse scanner that can optionally compute the compact and
varint compact sparse encoded sizes for the same nonzero page runs. For
`FIL_PAGE_TYPE_SYS`, `encoded_payload_size_for_page()` invokes that scanner
before materializing compact sparse bytes. The fast path publishes the existing
fill-sparse record only when the fill payload is smaller than the compact
winner and the compact winner would beat trailing-zero encoding. Otherwise the
unchanged compact, sparse, and trailing-zero fallback path runs.

The original `build_fill_sparse_zero_payload()` entry point remains a thin
wrapper over the same scanner with compact-size accounting disabled, so index
fill-sparse fallback behavior does not pay extra work after the compact
payload has already been selected.

## File Lifecycle

No file lifecycle change. Durable state remains in the existing ownerless
page-version WAL under the MyLite database directory.

## Embedded Lifecycle And API

No public API, open/close, or embedded runtime lifecycle behavior changes.

## Build, Size, And Dependencies

No dependency or build-profile change is introduced. The implementation shares
the existing fill-sparse scanner and avoids allocating the compact sparse
candidate for the hot SYS proof path when fill-sparse already wins.

## Test Plan

- Run the ownerless primitive page-log coverage to prove fill-sparse exact
  replay, checkpoint retention, and index prefilter behavior still pass.
- Run the reduced stats-enabled production probe and verify record counts and
  payload bytes remain stable while append encode time drops.
- Run focused production ownerless SQL selectors, hook coverage, stress
  coverage, production-build checks, formatter, and whitespace checks.

## Acceptance Criteria

- Focused primitive and SQL selectors pass.
- The representative stats-enabled production probe still reports exactly one
  SYS fill-sparse record per ownerless autocommit insert and unchanged page-log
  payload bytes.
- Append encode time improves relative to the pre-slice production sample.

## Implementation Evidence

The reduced 1000-row stats-enabled production probe preserved the same
`3010` single-row autocommit page-log append calls, `1000` fill-sparse records,
`1000` SYS records, `1746847` total payload bytes, and `72344` SYS payload
bytes as the pre-slice sample. Single-row autocommit append encode time moved
from `63.598 ms` in the pre-slice sample to `58.064 ms` with the shared
scanner. The same run reported `99.277 ms` total append time, compared with
the pre-slice `103.371 ms` sample.

The bulk phase preserved `1510` page-log append calls, `250` fill-sparse
records, `250` SYS records, and `1166401` total payload bytes. Bulk append
encode was `32.789 ms`.

Whole-run throughput ratios remain noisy because native commit,
page-publication, redo-leave, and payload-write timing vary between samples.
The next material performance target remains native commit/page-publication
cost and broader redo/checkpoint reconciliation, not startup or SQL dispatch.

## Risks And Open Questions

The fast path is deliberately limited to `FIL_PAGE_TYPE_SYS`. Index pages keep
the existing compact-payload prefilter because the representative insert path
proved index images are not fill-run dominated and broader fill scanning was
not a net win.
