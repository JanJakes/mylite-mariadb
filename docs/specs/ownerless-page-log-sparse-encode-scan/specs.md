# Ownerless Page-Log Sparse Encode Scan

## Problem

The ownerless page-version WAL append path encodes each page image before
writing it to `concurrency/mylite-concurrency.wal`. The current encoder first
scans the page from the end to compute the trailing-zero payload size, then
scans the page again to build a compact sparse-zero payload. On the ownerless
autocommit insert profile, the remaining page-log append cost is dominated by
encoding and payload writes for roughly three page-version records per insert.

The trailing-zero size is already implicit in the sparse run scan: it is the end
offset of the last nonzero run, or zero for an all-zero page. Reusing that value
removes one page scan for the common compact-sparse path without changing the
WAL record format.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::
  mtr_t::ownerless_page_writes_publish()` wraps ownerless page publication in a
  page-log append batch while scanning modified mini-transaction pages.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::
  mtr_t::ownerless_page_write_publish()` copies the physical page, initializes
  the page checksum, and calls `mylite_ownerless_innodb_publish_page_version()`.
- `packages/libmylite/src/database.cc::append_ownerless_page_version()` routes
  those page images into `mylite_ownerless_page_log_append_session_append()`
  while the MTR batch is active.
- `packages/libmylite/src/ownerless_page_log.cc::
  encoded_payload_size_for_page()` currently computes
  `trailing_zero_payload_size_for_page()` before trying compact sparse-zero and
  legacy sparse-zero encoders.
- `packages/libmylite/src/ownerless_page_log.cc::
  build_compact_sparse_zero_payload()` and `build_sparse_zero_payload()` already
  visit each nonzero run in order. The end of the last encoded run is the same
  trailing-zero payload size used by the trailing-zero fallback.
- Existing primitive tests cover compact sparse-zero encoding, legacy sparse
  fallback, record readback, latest-page lookup, and checkpoint/replay behavior.

## Design

Extend the sparse-zero builders with optional `out_trailing_size` and
`out_trailing_size_known` results:

```c++
bool build_compact_sparse_zero_payload(
    const void *page,
    uint32_t page_size,
    std::vector<unsigned char> *out_payload,
    uint64_t *out_trailing_size,
    bool *out_trailing_size_known);
```

The builders update `out_trailing_size` to the end offset of the last nonzero
run while scanning the page and mark it known after the scan completes. If the
sparse encoding cannot be used because it would not be smaller than the
original page, or because the page is all zero, the caller can still use the
derived trailing size and choose the trailing-zero fallback without a separate
reverse scan.

`encoded_payload_size_for_page()` keeps the same record-format selection:

1. prefer compact sparse-zero when it is smaller than the trailing-zero payload,
2. otherwise prefer legacy sparse-zero when compact is unavailable and legacy is
   smaller than trailing-zero,
3. otherwise use the trailing-zero payload when it is smaller than the full
   page, and
4. otherwise store the full page image.

The function should call `trailing_zero_payload_size_for_page()` only if both
sparse builders fail before deriving a trailing size. For normal InnoDB page
sizes, compact sparse-zero derives it in the first scan.

## Scope And Non-Goals

In scope:

- page-log encode scan reduction for compact and legacy sparse-zero paths,
- primitive coverage that still proves compact, legacy, and trailing-zero
  record readback,
- performance-probe evidence for append encode timing, and
- docs that keep the page-log format claim narrow.

Out of scope:

- changing the page-version WAL record format,
- changing crash publication order,
- batching page-log payload/header writes,
- dropping index, undo, or system page-version records, and
- replacing full-page checksums with partial-payload checksums.

## Compatibility Impact

No SQL behavior, C API behavior, PHP/mysqli behavior, wire-protocol behavior,
or directory layout changes. Existing WAL records remain readable because
flags, payload bytes, checksums, and record headers are unchanged.

## Directory And Lifecycle Impact

No files are added. The durable page-version WAL remains
`concurrency/mylite-concurrency.wal`. Closed-database copy/rebuild behavior is
unchanged because this slice only reduces in-memory encode work before writing
the same record shape.

## Native Storage Impact

Native InnoDB page publication still writes the same page-version records for
the same MTR page boundaries. Native redo, undo, checkpoints, and buffer-pool
state are unchanged.

## Build And Performance Impact

No dependency or build-profile changes. The expected gain is modest: the
ownerless autocommit profile should show lower page-log append encode time for
compact sparse-zero records, while payload-write bytes and record counts stay
the same. Real InnoDB page-version records often have nonzero trailer bytes, so
the old reverse trailing-zero scan can already stop quickly; measured
autocommit wins from this slice may therefore be small or lost in run-to-run
noise.

## Test And Verification Plan

- Extend `ownerless_primitives_test` page-log encoding coverage to include a
  trailing-zero-only record shape in addition to compact and legacy sparse
  readback.
- Run production and hook `mylite_ownerless_primitives_test`.
- Run a reduced production performance probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and compare page-log append
  encode timing.
- Run focused ownerless commit-path SQL coverage after rebuilding.
- Run `git diff --check`; use CI clang-format-18 style for touched
  `ownerless_page_log.cc` hunks.

## Acceptance Criteria

- Compact sparse-zero payloads are encoded and decoded identically to the
  previous format.
- Legacy sparse-zero fallback still works for shapes compact encoding cannot
  represent.
- Trailing-zero fallback still works when it is smaller than sparse encoding.
- The autocommit performance probe reports the same record counts and lower or
  comparable append encode time.
- No durable file format or crash ordering changes are introduced.

## Risks And Unresolved Questions

- This optimization is intentionally small and will not close the whole
  ownerless autocommit gap.
- The larger remaining page-log costs are payload writes and the need to publish
  user data/index, undo, and system page images for peer visibility and
  recovery. Dropping any of those records requires separate correctness proof.
