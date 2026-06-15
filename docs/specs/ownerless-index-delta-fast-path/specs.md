# Ownerless Index Delta Fast Path

## Problem

After no-dirty publish batching, the production ownerless attribution probe
still reported page-log encoding as a first-party hot path. A reduced 500-row
stats-enabled production sample at branch head `2cdceded` reported:

- `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=1506`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_session_append_calls=1504`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=54.815`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_records=503`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_records=470`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_index_payload_bytes=463975`;
- `mylite_perf_ownerless_insert_autocommit_page_log_append_undo_log_payload_bytes=274165`;
- `mylite_perf_summary_ownerless_autocommit_page_log_encode_ms_per_insert=0.110`.

The index-delta append path first built the standalone sparse/fill/index
payload, then built a delta and discarded the standalone payload when the
delta won. That is exact but expensive for the measured hot path: most index
records selected deltas, so the append path repeatedly paid for standalone
encoding that was never written.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `2cdceded`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  allocates an aligned page buffer, copies the buffer-pool source page into
  it, prepares checksums/write state, and only then calls
  `mylite_ownerless_innodb_publish_page_version()`.
- `mariadb/storage/innobase/buf/buf0flu.cc` ownerless dirty-page publication
  stores page images in `std::vector<byte>` or an allocated read buffer,
  prepares checksums/write state on that stable image, and then calls the same
  publish hook.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  transaction-image publication copies the saved image into a local
  `std::vector<byte>`, prepares checksums/write state, and calls the publish
  hook with that vector storage.
- `packages/libmylite/src/database.cc::ownerless_innodb_page_publish_hook()`
  appends the page synchronously before returning to MariaDB.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  already keeps a process-local index-delta base table keyed by page-log file
  identity, log offset, log generation, and page identity. Before this slice,
  the table stored the base page and durable record offset, but not the
  standalone encoded payload size.

## Design

Move the stable-page-source boundary to the ownerless publish hook contract:
callers of `mylite_ownerless_innodb_publish_page_version()` must pass a page
image that remains stable until the hook returns. Current MariaDB ownerless
callers satisfy this by passing copied, vector-backed, or allocated page
buffers, and the page-log append operation remains synchronous.

Store the standalone encoded payload size in each warmed index-delta base
slot. On the next same-identity index page, the append path now:

- snapshots the warmed base slot once;
- builds an index delta against that base before standalone encoding;
- fast-accepts the delta only when it is less than half the stored standalone
  payload size and below the configured fast payload limit;
- otherwise falls back to the previous exact path: build the current standalone
  payload, compare the delta against that exact current payload size, and write
  standalone when the delta is marginal.

The fast path therefore cannot increase payload size beyond the existing
half-standalone rule for accepted small deltas, and larger or uncertain deltas
still use the exact decision. Delta records remain non-chained and reference a
durable standalone base record offset, not process-local memory.

## Compatibility Impact

No SQL behavior, public C API, PHP API, wire-protocol behavior, page-log
record format, checkpoint policy, or native InnoDB semantics change. This is a
first-party hot-path decision change under the existing ownerless page-publish
contract. The performance probe adds a private
`index_delta_fast_records` counter to show how often standalone encoding was
skipped.

## Directory And Lifecycle Impact

No durable file or directory-layout change. The page-log append operation is
still synchronous and still writes payload bytes before the valid record
header. Process-local base-table state remains transient and can be dropped at
any time; losing it only disables the optimization until the next standalone
base warms.

## Native Storage Impact

Native InnoDB page publication still snapshots buffer-pool/native page state
before entering the MyLite page-log layer. The slice does not alter MTR LSN
assignment, checksum preparation, redo handoff, flush-list state, checkpoint,
or recovery behavior.

## Build And Performance Impact

Stats-off runtime removes one 16 KiB vector assignment for each published
`FIL_PAGE_INDEX` record and skips standalone sparse/fill encoding for
fast-accepted small index deltas. The expected gain is bounded because
undo-log compact sparse encoding, non-fast index deltas, native write-history,
and row-insert work remain.

## Test Plan

- Build production embedded primitive, performance probe, and ownerless SQL
  targets.
- Run ownerless primitive tests that cover index-delta readback, latest-page
  lookup, checkpoint rewrite, bounded base refresh, and the new fast-accepted
  small-delta counter.
- Run a reduced stats-enabled production performance probe and compare
  `*_page_log_append_encode_ms`, `index_delta_records`,
  `index_delta_fast_records`, index payload bytes, and throughput.
- Run focused ownerless SQL selectors covering history WAL proof,
  native-support page WAL elision, native/live reclaim, commit race, and active
  reader pressure.
- Run focused hook and stress subsets, production-build guards, format-check,
  and `git diff --check`.

## Acceptance Criteria

- `append_record_at_locked()` does not copy `FIL_PAGE_INDEX` pages into a
  second stable vector before encoding.
- Fast-accepted index deltas are counted separately and remain byte-exact
  through direct record read and latest-page lookup.
- The exact standalone comparison path remains available for non-fast deltas.
- Production probe output shows fast-accepted index deltas and lower page-log
  encode time in the reduced 500-row sample.
- Docs record that stable source images are provided by ownerless publish
  callers and that the page-log fast path is an optimization, not a durable
  format change.

## Implementation Evidence

- `test_page_log_fast_encodes_small_index_delta_payloads()` covers the
  fast-accepted small-delta counter, durable delta flag selection, byte-exact
  direct record read, and byte-exact latest-page lookup.
- The full ownerless primitive CTest selector passed under `php-embedded-prod`.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported:
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_calls=1506`;
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_encode_ms=30.980`;
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_index_records=503`;
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_records=470`;
  - `mylite_perf_ownerless_insert_autocommit_page_log_append_index_delta_fast_records=402`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_encode_ms_per_insert=0.062`.

Compared with the pre-slice 500-row sample above, page-log append encode time
dropped from `54.815 ms` to `30.980 ms` while append count and index-delta
selection stayed stable.

## Risks And Follow-Up

Future publish hook callers must preserve the stable-page-source contract. A
future path that passes a live mutable buffer-pool page directly into the
page-log layer must copy before calling the hook.

The fast path does not reduce undo-log compact sparse encoding, non-fast index
deltas above the bounded threshold, native InnoDB commit/write-history cost, or
redo/checkpoint reconciliation work. Those remain follow-up performance and
correctness targets.
