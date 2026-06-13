# Ownerless Index Delta First Base

## Problem

Current production attribution after the single-row page-log append-batch
slice still shows ownerless write cost in page-version publication and page-log
encoding. A reduced Release probe with `MYLITE_PERF_INSERT_ITERATIONS=500` and
page-publish stats enabled reported:

- `mylite_perf_summary_ownerless_autocommit_page_log_payload_bytes_per_insert=1541.174`;
- `mylite_perf_summary_ownerless_autocommit_page_log_index_payload_bytes_per_insert=927.956`;
- `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_records_per_insert=0.940`;
- `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_fast_records_per_insert=0.804`;
- `mylite_perf_summary_ownerless_autocommit_page_log_undo_log_payload_bytes_per_insert=539.904`;
- `mylite_perf_summary_ownerless_autocommit_page_log_encode_ms_per_insert=0.059`.

The existing index-delta format is already durable, non-chained, and rewritten
as standalone records during checkpoint. The remaining representative simple
insert path still writes standalone index records while waiting for eight
standalone observations before a base can seed deltas. That warm-up is now
more conservative than the implemented correctness proof requires.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` ownerless publish paths call the
  MyLite page-version hook after preparing a stable committed page image.
- `packages/libmylite/src/ownerless_page_log.cc` stores each index delta
  against an absolute durable base record offset. `decode_index_delta_payload()`
  rejects a base that is itself a delta record, rejects mismatched page
  identity/size, reconstructs the full page, and validates the delta record's
  full-page checksum.
- `checkpoint_locked()` and
  `checkpoint_preserving_oldest_snapshot_locked()` call
  `read_standalone_or_rewrite_delta_payload()`, so retained delta records are
  decoded and rewritten as standalone records at their compacted offsets.
- `note_index_delta_base_after_successful_append()` only records standalone
  index-page appends as bases and only increments a bounded
  `delta_records_since_base` counter for successful delta records.
- `index_delta_base_snapshot()` already refuses a base once the bounded
  `delta_records_since_base` limit is reached, forcing the next eligible append
  through the standalone path and refreshing the base.

## Design

Lower `k_index_delta_base_min_standalone_observations` from eight to one.

The policy remains conservative in the ways that matter for correctness:

- every delta still points to an earlier durable standalone record in the same
  page-log file identity, log offset, generation, and page identity;
- deltas remain non-chained;
- system-tablespace index pages remain excluded;
- a delta is selected only when it is less than half the remembered standalone
  payload size, or after exact standalone encoding proves the delta is less
  than half the current standalone payload;
- after the bounded delta count, the next append is standalone and refreshes
  the base;
- checkpoint rewrites any retained delta as a standalone record.

## Scope And Non-Goals

In scope:

- process-local index-delta warm-up policy;
- primitive tests proving a single standalone base can seed deltas;
- production probe evidence for index payload and encode counters.

Out of scope:

- new page-log record flags or formats;
- undo-log, SYS, BLOB, space-metadata, or native-support proof changes;
- changing checkpoint retention semantics;
- changing SQL, C API, mysqli, PHP, WordPress, or wire-protocol behavior;
- claiming ownerless concurrency complete.

## Compatibility Impact

No user-visible compatibility change. Existing page-log records remain
readable, and new records use the existing standalone and index-delta encodings.
The change only chooses existing delta records sooner after one durable
standalone base has been written.

## Directory And Lifecycle Impact

No new files or directory layout changes. Durable state remains in the existing
ownerless page-version WAL under the MyLite database directory. The base table
is process-local and can be discarded at any time; losing it only disables the
optimization until the next standalone index record is appended.

## Native Storage Impact

No native InnoDB page, redo, undo, checkpoint, or dictionary format changes.
Published page-log records still reconstruct byte-exact page images and validate
the full-page checksum stored in the record header.

## Build And Performance Impact

The change removes unnecessary standalone warm-up records for repeated
file-per-table index pages in the simple ownerless insert path. It should reduce
index payload bytes and may reduce encode time by increasing fast-accepted index
deltas. It does not address undo-log payload, native history-proof page
publication, row-insert cost, or broader redo/checkpoint reconciliation.

## Test And Verification Plan

- Update primitive index-delta tests so one standalone base can seed a delta.
- Keep byte-exact direct record reads, latest-page lookup, checkpoint rewrite,
  post-checkpoint fallback, fast-delta counter, and bounded base-refresh
  coverage.
- Run ownerless primitive coverage under `php-embedded-prod`.
- Run focused ownerless SQL selectors covering history WAL proof,
  native-support page WAL elision, visible-fast insert publication, and FK
  fast-path cache behavior.
- Run a reduced stats-enabled production performance probe and a stats-off
  throughput sanity probe.
- Run production-build guards, format check, and `git diff --check`.

## Acceptance Criteria

- A single standalone index record can seed a later delta record.
- Direct record read, latest-page lookup, and checkpoint rewrite remain
  byte-exact.
- Bounded base refresh still forces a standalone record after the configured
  delta count and then allows a later delta from the refreshed base.
- Production attribution shows fewer index payload bytes per ownerless
  autocommit insert without a bulk-throughput collapse.

## Implementation Evidence

- `k_index_delta_base_min_standalone_observations` is now `1`, so the append
  path can use a durable standalone base immediately while keeping the existing
  non-chained delta and base-refresh rules.
- `test_page_log_encodes_index_delta_payloads()` now proves one standalone
  index record can seed a delta and keeps byte-exact direct/latest readback plus
  checkpoint rewrite coverage.
- `test_page_log_fast_encodes_small_index_delta_payloads()` now warms with one
  standalone base and proves fast-accepted delta counting and byte-exact
  readback.
- `test_page_log_refreshes_index_delta_base()` now warms with one standalone
  base and proves the first `32` later records are deltas, the next record is a
  standalone refresh, and the following record can delta against the refreshed
  base.
- Local production verification on 2026-06-13 passed:
  `libmylite.ownerless-primitives`, the focused adjacent ownerless SQL subset
  for history WAL proof/native-support WAL elision/visible-fast insert/FK fast
  path cache, direct `stress`, hook `visible-publish-crash` and
  `visible-checkpoint-crash`, production-build guards, `format-check-prod`, and
  `git diff --check`.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_INSERT_ITERATIONS=500` reported
  `mylite_perf_summary_ownerless_autocommit_page_log_index_payload_bytes_per_insert=829.054`,
  `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_records_per_insert=0.962`,
  and
  `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_fast_records_per_insert=0.820`.
  The pre-slice same-shape sample reported `927.956` index bytes per insert,
  `0.940` index deltas per insert, and `0.804` fast deltas per insert.
- A repeat stats-off throughput sanity probe reported ownerless autocommit
  `1391.34 ops/s` (`0.4320` ordinary) and ownerless bulk rows
  `3172.95 ops/s` (`0.2777` ordinary). Wall-clock probe samples remain noisy,
  so the byte/count counters are the primary performance evidence.

## Risks And Follow-Up

Earlier deltas may occasionally compare against a less representative first
base. The existing half-standalone rule, exact fallback path, and bounded
refresh policy limit that risk. Remaining performance work should continue
toward undo-log payload, native-support history-proof publication, and broader
redo/checkpoint recovery rather than more warm-up tuning alone.
