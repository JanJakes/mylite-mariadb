# Ownerless Delta Note Slot Reuse

## Problem Statement

The ownerless page-log append path already looks up a process-local delta-base
slot before deciding whether an index, undo, or history-rseg page can be encoded
as a non-chained delta. After a successful append, the same path immediately
hashes and probes the same cache again so it can update the base slot or
increment its bounded delta count.

Current production attribution still points at native page-publication and
history-proof volume as the larger ownerless write target, but the reduced
current-head sample showed `0.023 ms/insert` in
`page_log_delta_base_note_ms` for one-row ownerless autocommit and `2.000 ms`
in standalone size probes for the bulk sample. Reusing the already-found slot is
a bounded first-party cleanup that reduces duplicate cache work without changing
page-version records or recovery rules.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` passes stable committed InnoDB page
  images to the MyLite ownerless page-version hook after the commit LSN is
  installed.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  classifies each append, calls `index_delta_base_snapshot()` before encoding,
  writes the payload and record header, then calls
  `note_index_delta_base_after_successful_append()` after the record header is
  durable.
- `index_delta_base_snapshot()` already proves the matching process-local slot
  under `index_page_delta_base_mutex`; the slot identity includes log device,
  inode, log offset, generation, delta flag, space id, page number, and page
  size.
- `note_index_delta_base_after_successful_append()` must still be authoritative
  after the record header is written. It cannot update the cache before a
  failed append, and it must tolerate another thread mutating the volatile
  cache between the snapshot and note phases.

## Design

Carry the found slot index in `IndexPageDeltaBaseSnapshot`. When the appended
record is a delta, let the note helper try that preferred slot under the same
mutex before recomputing the fingerprint and probing the cache.

The preferred-slot path is accepted only when the slot still matches the full
identity and page size, still has a page buffer, and its bounded delta counter
can be incremented. If the slot no longer matches, the helper falls back to the
existing fingerprint/probe loop. Standalone records continue to use the existing
base insert/refresh path.

This preserves:

- non-chained delta records against a durable standalone base offset;
- the existing `k_index_delta_base_max_delta_records` refresh boundary;
- exact fallback standalone-size estimate refresh;
- checkpoint rewrite and byte-exact replay behavior; and
- process restart/cache-eviction semantics.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli, wire-protocol, storage-format, page-log
record-format, checkpoint-format, or directory-layout behavior changes. The
change only reuses a process-local cache slot after the same append already
proved it.

## Native Storage Impact

Native InnoDB pages, redo, undo, mini-transaction commit, and crash recovery are
unchanged. The page-log payload and checksum for every appended record stay the
same.

## Build And Performance Impact

No new dependencies, public symbols, or durable state. The expected impact is a
small CPU reduction in the page-log delta-base note phase for accepted delta
records, especially repeated history-rseg, undo, and index deltas. This does
not reduce page-version publication count, native-support proof count, history
flush work, or redo/checkpoint recovery scope.

## Test And Verification Plan

- Build production ownerless primitive, cross-process SQL, and embedded
  performance targets.
- Run `libmylite.ownerless-primitives` to cover delta record selection,
  byte-exact latest-page readback, and checkpoint rewrite.
- Run focused ownerless SQL selectors for history WAL proof,
  native-support page WAL elision, visible-fast multi-row inserts, and
  uncommitted peer visibility.
- Run a reduced stats-enabled production performance probe and compare
  append counts, payload bytes, delta counts, and delta-base note timing.
- Run production build guards, `format-check-prod`, and `git diff --check`.

## Verification Evidence

Focused production build:

```sh
cmake --build --preset php-embedded-prod --target \
  mylite_ownerless_primitives_test \
  mylite_embedded_performance_probe \
  mylite_ownerless_cross_process_sql_test
```

Focused ownerless correctness:

```sh
ctest --preset php-embedded-prod \
  -R '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$' \
  --output-on-failure
```

This passed 5/5 locally. A reduced stats-enabled production probe with 500
one-row ownerless autocommit inserts and 500 four-row bulk autocommit statements
kept the same page-version and WAL-shape counters:

- one-row page versions per insert: `3.008`;
- one-row native-support published pages per insert: `2.004`;
- one-row page-log append calls per insert: `3.018`;
- one-row ownerless page-log payload bytes per insert: `1082.168`;
- one-row ownerless index/undo/history-rseg delta records: `481`, `376`,
  and `374`;
- bulk page versions per row: `1.508`;
- bulk native-support published pages per row: `0.504`;
- bulk page-log append calls: `759`;
- bulk ownerless page-log payload bytes: `452983`;
- bulk ownerless index/undo/history-rseg delta records: `482`, `124`, and
  `125`.

The measured note phase remained small and noisy. The prior current-head sample
showed `0.023 ms/insert` in one-row `page_log_delta_base_note_ms`; the slot-reuse
sample showed `0.022 ms/insert`. The prior bulk sample showed `0.208 ms` total
in `page_log_delta_base_note_ms`; the slot-reuse sample showed `0.173 ms`.
Those numbers support the slice as a narrow CPU cleanup, not as a material fix
for the larger ownerless write-throughput gap.

## Acceptance Criteria

- `IndexPageDeltaBaseSnapshot` records the matched slot index.
- Successful delta appends try the preferred slot before falling back to the
  existing cache probe.
- A changed or evicted preferred slot falls back safely.
- Primitive and focused SQL coverage passes with unchanged page-log payload
  counts.
- The slice is documented as a bounded CPU cleanup, not a completion of
  ownerless write-throughput work.

## Risks And Follow-Up

The preferred slot is only a volatile process-local hint. It must never replace
the full identity check, and it must never be required for correctness. Larger
remaining performance and correctness targets remain native history-proof
publication volume, broader redo/checkpoint reconciliation, DDL/file lifecycle
recovery, active-reader pressure breadth, and external MariaDB/RQG stress.
