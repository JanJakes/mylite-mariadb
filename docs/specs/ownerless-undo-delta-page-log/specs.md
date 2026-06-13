# Ownerless Undo Delta Page Log

## Problem

After the index-delta first-base slice, current production attribution still
showed one `FIL_PAGE_UNDO_LOG` page-version record per ownerless autocommit
insert:

- total page-log payload: `1442.294` bytes per insert;
- index payload: `829.066` bytes per insert;
- undo-log payload: `539.910` bytes per insert;
- SYS payload: `72.192` bytes per insert;
- history-proof publication: `1.000` rollback-segment page and `1.000` undo
  page per insert.

The undo history-proof page remains required evidence for the current native
commit handoff, but the page image itself often repeats by page identity. The
bounded slice is to compress repeated undo-log page records using the existing
durable non-chained delta shape, without eliding the proof page or changing
SQL/API behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines InnoDB page type
  metadata at offset 24 and `FIL_PAGE_UNDO_LOG` as page type `2`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes ownerless page images
  after committed page LSN/checksum preparation. The MyLite page-log checksum
  can therefore continue to validate the reconstructed full page image.
- `packages/libmylite/src/ownerless_page_log.cc` already decodes index deltas
  through the same full-page read path used by direct reads, latest-page
  lookup, page-type classification, payload validation, replay, and checkpoint.
- `checkpoint_locked()` and
  `checkpoint_preserving_oldest_snapshot_locked()` already call
  `read_standalone_or_rewrite_delta_payload()`, which is the right place to
  keep retained base-dependent records independently decodable after
  compaction.

## Scope And Non-Goals

In scope:

- first-party ownerless page-log internals;
- a new internal undo-delta record flag;
- shared delta-base cache eligibility for `FIL_PAGE_UNDO_LOG` records;
- primitive tests for byte-exact direct/latest readback and checkpoint rewrite;
- performance-probe counters for undo delta records, fast records, and payload.

Out of scope:

- eliding history-proof pages;
- changing native undo, redo, checkpoint, purge, or rollback-segment formats;
- using deltas for BLOB, SYS, TRX_SYS, allocation, dictionary, or other page
  classes;
- changing public C API, SQL, mysqli, PHP, WordPress, or wire-protocol
  behavior;
- claiming ownerless concurrency complete.

## Design

Add `k_record_flag_undo_delta_payload` as a separate internal page-log record
flag. Undo deltas reuse the existing non-chained payload layout:

1. absolute `uint64` base record offset;
2. fixed `uint16` run count;
3. varuint16 gap/run-size pairs;
4. raw replacement bytes for each changed run.

The process-local base cache is keyed by page-log file device/inode, page-log
offset, page-log header generation, delta class, `(space_id, page_no)`, and
page size. A delta is eligible only after one durable standalone base record in
the same log generation. Delta records never become bases. Checkpoint invalidates
matching process-local bases before compaction and rewrites any retained delta
as a standalone record at the compacted offset.

The append path now asks a generic page-delta eligibility helper whether the
page is an eligible `FIL_PAGE_INDEX` or `FIL_PAGE_UNDO_LOG` page. Index pages
keep the existing system-tablespace exclusion. Undo-log pages use the same
half-standalone-size rule and the same bounded fast-path payload limit as index
pages. All delta record headers retain the full page checksum, so direct reads,
latest-page lookup, replay validation, and page-type classification reconstruct
the full page before accepting the record.

## Compatibility Impact

No SQL or public API behavior changes. This adds a new internal ownerless
page-log flag; older binaries that do not know the flag reject those records
through existing known-flag validation. Durable state remains inside the
MyLite-owned database directory.

## Directory And Lifecycle Impact

No new files are introduced. The base cache is transient process-local
optimization state. Dropping it only forces future records back through the
standalone encoding until a new durable base is appended.

## Build, Size, And Dependencies

No dependency or build-profile change. The code reuses the existing page-log
delta encoder/decoder and adds one internal flag plus a few diagnostic counters.

## Test And Verification Plan

- Extend `ownerless_primitives_test` to prove:
  - one standalone undo-log record can seed a later undo delta;
  - the undo delta uses the undo flag, not the index flag;
  - direct record read and latest-page lookup reconstruct byte-exact pages;
  - checkpoint rewrites a retained undo delta as a standalone record;
  - post-checkpoint append falls back until a fresh base exists.
- Extend performance-probe output with raw and per-insert undo-delta counters.
- Run focused primitive, history-proof/native-support, visible-fast, and
  performance verification.
- Run production-build guards and whitespace checks before commit.

## Acceptance Criteria

- Undo delta records never depend on process-local memory for replay.
- Retained undo deltas are independently decodable after checkpoint.
- Existing index delta, sparse, compact sparse, varint sparse, fill sparse, and
  full records remain readable.
- Production attribution shows undo-log payload reduction while preserving the
  existing history-proof publication counts.

## Implementation Evidence

- `test_page_log_encodes_undo_delta_payloads()` proves undo-delta selection,
  byte-exact direct/latest readback, checkpoint rewrite to standalone, and
  post-checkpoint fallback.
- Local production primitive verification passed:
  `ctest --test-dir build/php-embedded-prod -R '^libmylite\\.ownerless-primitives$'
  --output-on-failure`.
- Adjacent ownerless SQL verification passed for
  `ownerless-single-owner-history-wal-proof`,
  `ownerless-single-owner-native-support-page-wal-elision`,
  `ownerless-single-owner-multi-row-insert-visible-fast-path`, and
  `ownerless-insert-fk-fast-path-cache`.
- Hook-built `visible-publish-crash` and `visible-checkpoint-crash` selectors
  passed directly.
- The `ownerless-stress` build's `libmylite.ownerless-cross-process-stress`
  selector passed.
- Production-build guard, `format-check`, and whitespace checks passed.
- A reduced 500-row production attribution probe on 2026-06-14 reported:
  - `mylite_perf_summary_ownerless_autocommit_page_log_payload_bytes_per_insert=1112.380`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_undo_log_payload_bytes_per_insert=210.006`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_undo_delta_records_per_insert=0.752`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_undo_delta_fast_records_per_insert=0.728`;
  - `mylite_perf_summary_ownerless_autocommit_page_log_undo_delta_payload_bytes_per_insert=103.870`;
  - `mylite_perf_summary_ownerless_autocommit_native_support_published_history_proof_rseg_pages_per_insert=1.000`;
  - `mylite_perf_summary_ownerless_autocommit_native_support_published_history_proof_undo_pages_per_insert=1.000`.

The preceding same-shape current-head probe reported total page-log payload
`1442.294` bytes per insert and undo-log payload `539.910` bytes per insert.
A stats-off 500-row throughput sanity probe reported ownerless autocommit
`1558.38 ops/s`, ordinary autocommit `3240.52 ops/s`, and an ownerless ratio
of `0.4809`; throughput samples remain host-noisy, so byte/count counters are
the primary evidence.

## Risks And Follow-Up

Undo deltas reduce the payload cost of the current proof page; they do not
replace the proof protocol. Remaining high-impact performance work should focus
on native commit/page-publication cost, non-fast delta encoding, a smaller or
different history-proof representation, and broader redo/checkpoint recovery.
