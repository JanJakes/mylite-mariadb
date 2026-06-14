# Ownerless Delta Base Shared Snapshot

## Problem

The ownerless page-log delta path now covers repeated `FIL_PAGE_INDEX` and
`FIL_PAGE_UNDO_LOG` records with durable non-chained delta records. The hot
append path can fast-accept small deltas before building a standalone payload,
but every delta candidate still copied the cached 16 KiB standalone base page
out of the process-local base table before encoding.

That copy is not durable evidence. The durable evidence is the base record
offset stored in the delta payload plus the non-chained decode rule that
rejects delta bases. The next bounded performance slice is therefore to remove
the process-local base-page copy while keeping the same page-log format,
checkpoint rewrite behavior, and recovery semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc` owns the first-party
  page-log delta base cache.
- `append_record_at_locked()` snapshots a matching base before trying the fast
  small-delta path and before the exact fallback comparison.
- `build_index_delta_payload()` only reads the base page while building a
  replacement run list; it does not mutate the base.
- `decode_page_delta_payload()` reads the durable base record by offset and
  rejects base records that are themselves deltas, so no reader or recovery
  path depends on process-local base-cache memory.
- `checkpoint_locked()` and
  `checkpoint_preserving_oldest_snapshot_locked()` continue to rewrite retained
  delta records as standalone records through
  `read_standalone_or_rewrite_delta_payload()`.

## Scope And Non-Goals

In scope:

- Store process-local delta base pages as immutable shared vectors.
- Make delta-base snapshots copy a shared reference instead of copying the
  16 KiB base page bytes.
- Rename the delta encoder helper so it reflects both index and undo pages.
- Extend primitive coverage to assert fast-accepted undo deltas explicitly.

Out of scope:

- Changing the ownerless page-log record format or delta payload layout.
- Letting delta records chain through other delta records.
- Eliding history-proof records.
- Changing native InnoDB commit, redo, undo, checkpoint, or recovery behavior.
- Reopening multi-row append-session deferral, which has prior regression
  evidence.

## Design

`IndexPageDeltaBaseSlot` now stores its base page as
`std::shared_ptr<const std::vector<unsigned char>>`. A successful standalone
append replaces the slot with a new immutable vector and durable base-record
offset. A matching delta snapshot copies that shared pointer under the existing
base-cache mutex, then builds the delta outside the mutex exactly as before.

The base cache remains process-local optimization state:

- losing a slot still forces standalone encoding until a new base is written;
- a delta record still stores only a durable base-record offset and byte runs;
- readers and checkpoint recovery still reconstruct from durable WAL records;
- checkpoint invalidation still drops process-local slots for the compacted log.

The encoder helper name changes from `maybe_encode_index_delta_payload()` to
`maybe_encode_page_delta_payload()` because the same path now deliberately
covers both `FIL_PAGE_INDEX` and `FIL_PAGE_UNDO_LOG` delta classes.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli, wire-protocol, or on-disk native
storage behavior changes. Existing ownerless page-log records remain readable,
and newly written records use the same flags and payload encodings as before.

## Directory And Lifecycle Impact

No new files or directory layout changes. The shared base pages are transient
process memory and are discarded on process exit, checkpoint invalidation,
table overflow, or allocation failure. Durable state remains in
`concurrency/mylite-concurrency.wal`.

## Native Storage Impact

No native InnoDB page format, redo, undo, checkpoint, or recovery change.
Ownerless publish callers still pass stable page images into the synchronous
page-log append path.

## Build, Size, License, And Dependencies

No new dependency or license impact. `ownerless_page_log.cc` already includes
`<memory>`; the implementation changes a process-local cached vector into an
immutable shared vector.

## Test And Verification Plan

- Build `mylite_ownerless_primitives_test` with `php-embedded-prod`.
- Run `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`.
- Run focused ownerless SQL selectors for history WAL proof,
  native-support WAL elision, and multi-row visible-fast coverage.
- Run a reduced production attribution probe when practical to confirm record
  counts remain stable and encode timing does not regress.
- Run `tools/check-ci-production-builds`.
- Run `git diff --check`.

## Acceptance Criteria

- Index and undo delta records still read back byte-for-byte through direct
  record reads and latest-page lookup.
- Retained delta records still checkpoint as standalone records.
- Fast-accepted undo deltas are explicitly covered by primitive counters.
- Delta snapshots no longer copy cached base-page bytes from the base table.
- The durable non-chained delta contract is unchanged.

## Implementation Evidence

- `IndexPageDeltaBaseSlot` stores cached base pages as
  `std::shared_ptr<const std::vector<unsigned char>>`, and
  `IndexPageDeltaBaseSnapshot` copies only that shared reference before
  encoding.
- The delta encoder helper is now named `maybe_encode_page_delta_payload()` and
  still accepts only the existing index and undo delta flags.
- `test_page_log_encodes_undo_delta_payloads()` now asserts the fast-accepted
  undo-delta counter for the eligible pre-checkpoint delta and asserts zero
  fast undo deltas after checkpoint invalidation forces standalone fallback.
- Focused production primitive CTest passed under `php-embedded-prod`.
- Focused ownerless SQL selectors for history WAL proof, native-support page
  WAL elision, and multi-row visible-fast coverage passed under
  `php-embedded-prod`.
- A reduced 500-row stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported:
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=1506`;
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=19.822`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_encode_ms_per_insert=0.040`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_payload_bytes_per_insert=1112.408`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_records_per_insert=0.962`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_fast_records_per_insert=0.820`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_undo_delta_records_per_insert=0.752`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_undo_delta_fast_records_per_insert=0.726`.
- The same short local probe reported ownerless autocommit at
  `1186.18 ops/s` versus ordinary autocommit at `1127.10 ops/s`; this
  throughput result is recorded only as a sanity sample because short local
  throughput runs remain noisy.

## Risks And Follow-Up

- `std::shared_ptr` reference counting replaces a 16 KiB memcpy on each delta
  candidate; the hot-path benefit should be measured in production probes, but
  the semantic risk is low because readers already depend only on durable WAL
  records.
- The base-cache mutex remains global to the page-log process-local table.
  This slice reduces time under the mutex and snapshot copy cost, but it does
  not redesign the table for high cross-database contention.
- Broader native commit/page-publication and redo/checkpoint reconciliation
  remain the larger performance and correctness targets.
