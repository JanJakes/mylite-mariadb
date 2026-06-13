# Ownerless Index Delta Page Log

## Goal

Reduce ownerless write-path page-log payload for repeated `FIL_PAGE_INDEX`
pages by encoding changed byte ranges against a retained full base record.
The preceding attribution slice showed representative simple inserts publish
mostly duplicate index page identities with about `40` changed bytes per insert
while the current page-log payload still writes about `1779` index bytes per
insert.

## Non-Goals

- Do not use deltas for native-support pages, SYS proof pages, undo-log pages,
  BLOB pages, system-tablespace dictionary/index pages, or non-index page
  classes in this slice.
- Do not change SQL, public C API, mysqli, WordPress, or wire-protocol
  behavior.
- Do not make delta records depend on process-local memory for replay.
- Do not allow checkpoint to retain a delta record after its base record has
  been discarded.
- Do not claim ownerless concurrency complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines `FIL_PAGE_TYPE` at
  offset 24, `FIL_PAGE_DATA` at offset 38, and `FIL_PAGE_INDEX` as the
  uncompressed B-tree page type.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` writes the committed page LSN and
  publishes ownerless page images from the committed buffer-pool frame after
  checksum preparation. The page-log record checksum therefore continues to
  cover the reconstructed full page image.
- `packages/libmylite/src/ownerless_page_log.cc` currently treats page-log
  records as independently decodable from their payload and record header.
  `find_latest_in_snapshot_range()`, `read_record_at_locked()`,
  `record_payload_status()`, `record_requires_oldest_snapshot_boundary()`, and
  replay/checkpoint validation all call the same payload decode path or the
  page-type extraction helper.
- `checkpoint_locked()` and
  `checkpoint_preserving_oldest_snapshot_locked()` rewrite retained records by
  copying each record header and payload verbatim today. A base-dependent delta
  record cannot safely keep that behavior, because checkpoint may discard the
  base record while retaining a newer record that is still visible to an
  active snapshot.
- The current page-log append session already passes the physical record
  offset to `append_record_at_locked()`. That offset can be embedded in a delta
  payload as an absolute base-record offset without changing the fixed
  64-byte record header.

## Compatibility Impact

No user-visible SQL/API compatibility change is introduced. The durable
ownerless page-log format changes by adding a new internal record flag, so
older binaries that do not know the flag must reject those records through the
existing known-flag validation. The format remains inside the MyLite database
directory and is not a public API.

## Design

Add an index-delta payload flag for `FIL_PAGE_INDEX` records. The delta payload
contains:

1. an absolute `uint64` base record offset,
2. a fixed `uint16` run count,
3. varuint16 gap/run-size pairs,
4. raw replacement bytes for each changed run.

The base record must be a previously appended full, non-delta record for the
same page-log file identity, page-log offset, page-log generation,
`(space_id, page_no)`, and page size. The append path keeps a bounded
process-local base table with full page copies and their base record offsets.
The table is an optimization cache only; after process restart, table
overflow, or checkpoint generation change, the next page for that identity is
written through the existing standalone encoding and becomes the new base.

When appending an index page:

- copy the index page bytes once before standalone encoding, delta selection,
  checksum calculation, page-type stats, payload write, and base-cache update,
  so an expensive delta decision cannot observe a different live buffer-pool
  image than the durable payload/checksum pair;
- build the existing standalone encoded payload first,
- skip delta selection for the InnoDB system tablespace (`space_id=0`) because
  DDL/dictionary churn updates those pages in short, correctness-sensitive
  bursts that are not the representative DML hot-page target,
- if a same-identity base has at least eight standalone observations, build a
  delta against the full base page,
- choose the delta only when its payload is less than half the standalone
  payload; marginal deltas are written standalone so the base refreshes,
- when a standalone index record is written successfully, replace the base
  slot with that full page and record offset,
- when a delta record is written successfully, leave the base slot unchanged
  so delta records do not form recursive chains.

Read paths reconstruct deltas by reading and decoding the base record first,
then applying changed runs and validating the existing full-page checksum from
the delta record header. This keeps `find_latest`, direct record reads,
replay validation, and page-type classification on one checksum path.

Checkpoint paths must never retain base-dependent deltas verbatim. Whenever a
retained record is a delta, checkpoint decodes the full page and writes a
standalone page-log record at the new compacted offset using the existing
non-delta encoding selection. The retained-record callback receives the new
offset of that standalone replacement. This avoids retaining base records just
for delta dependencies and keeps post-checkpoint records independently
decodable.

## File Lifecycle

No new files are introduced. Delta records live in the existing ownerless
page-version WAL inside the MyLite database directory. Process-local base
tables are transient and can be dropped at any time; doing so only disables
the optimization until the next standalone base is written.
The cache key includes the page-log header generation, and local checkpoints
also invalidate matching slots, so stale base offsets are not reused after
page-log compaction.

## Embedded Lifecycle And API

No public API, directory-open, close, or embedded runtime lifecycle behavior
changes. Ownerless open/recovery continues to validate page-log records through
the existing page-log scan/replay paths.

## Build, Size, And Dependencies

No dependency or build-profile change. The implementation adds a first-party
payload encoder/decoder and bounded process-local base table to the page-log
module.

## Test Plan

- Extend ownerless primitive coverage to prove:
  - repeated same-identity index pages can select the delta payload,
  - the decoded latest page and direct record read are byte-exact,
  - a different page identity or page size falls back to standalone encoding,
  - checkpoint rewrites retained delta records as standalone records,
  - checkpointed records remain byte-exact after base records are discarded,
  - active-snapshot boundary behavior for index records remains busy without a
    boundary and succeeds with a retained boundary.
- Emit delta record counts and payload bytes from the performance probe so the
  ownerless autocommit summary shows actual payload reduction.
- Run focused production primitive and ownerless SQL selectors.
- Run the reduced stats-enabled production performance probe and record index
  delta selection, payload bytes, append time, encode time, and throughput.
- Run hook ownerless coverage, reduced ownerless stress, production-build
  guards, formatter, tidy, and whitespace checks.

## Acceptance Criteria

- Delta records are never required to decode from process-local memory.
- Checkpoint-retained records are independently decodable after compaction.
- Existing full, trailing-zero, sparse, compact-sparse, varint compact-sparse,
  and fill-sparse records remain readable.
- Representative ownerless autocommit insert stats show lower index payload
  bytes than the `1779` bytes per insert baseline when deltas are selected.
- Docs and compatibility notes state the measured result and any remaining
  performance target.

## Implementation Evidence

- `test_page_log_encodes_index_delta_payloads()` covers durable delta flag
  selection, byte-exact direct record reads, byte-exact latest-page reads,
  short-burst standalone behavior before warm-up, checkpoint rewrite of a
  retained delta as a standalone record after its base is discarded, and
  post-checkpoint cache invalidation/fallback.
- The full ownerless primitive CTest selector passed under
  `php-embedded-prod`.
- A reduced stats-enabled production performance probe with
  `MYLITE_PERF_INSERT_ITERATIONS=100` selected `90` index delta records over
  `100` ownerless autocommit inserts. It reported
  `mylite_perf_summary_ownerless_autocommit_page_log_index_payload_bytes_per_insert=598.580`
  and
  `mylite_perf_summary_ownerless_autocommit_page_log_index_delta_payload_bytes_per_insert=539.870`,
  compared with the preceding `1779` index bytes-per-insert baseline. The
  timing sample was collected while another PHPUnit workload was active on the
  host, so the byte/count counters are the evidence from that run rather than
  the wall-clock throughput.

## Risks And Open Questions

The process-local base table only helps after a process has written enough
standalone records for that index identity to pass the warm-up threshold. That
is acceptable for a bounded slice, because the attribution sample showed
repeated same-process identities in the embedded write path, while DDL
dictionary refreshes can produce short bursts that should stay independently
decodable. The first writer after process restart, table overflow, checkpoint
generation change, or system-tablespace DDL churn still writes standalone page
images. The non-chained design keeps decode and checkpoint bounded but
requires periodic standalone refreshes when the cumulative difference from the
full base stops being a large payload win. Delta checkpoint rewrite must
remain correct; retaining base-dependent records verbatim would be a
correctness bug.
