# Ownerless Proof-Only Readable WAL Scan

## Problem

Ownerless native-support history proof now writes proof-only page-log metadata
records with no readable page payload. The open and close paths still classify
any page-log bytes after the page-log header as payload records. That
file-size-only test can enable ordinary native page-log read handling and
retained-payload shutdown policy even when the log contains only proof-only
metadata records.

The slice adds a precise readable-record scan for startup/open-close decisions:
proof-only records remain durable and uncheckpointed, but they no longer count
as readable page-version payload records.

## Source Findings

- Base source authority: MariaDB 11.8 LTS initial import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc` stores proof-only records as
  ordinary page-log headers with `MYLITE_OWNERLESS_PAGE_LOG_RECORD_PROOF_ONLY`,
  `MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE`, zero payload bytes,
  and zero checksum.
- The same page-log module already skips proof-only records for direct record
  reads, latest-page reads, replay callbacks, and checkpoint retained-record
  callbacks.
- `packages/libmylite/src/database.cc` uses
  `ownerless_page_log_has_payload_records()` at ordinary native open and final
  ownerless close, but the helper currently checks only whether the WAL file is
  larger than the initialized page-log header.

## Design

Add a first-party page-log scan API that answers whether a page-log snapshot
contains at least one complete, non-proof-only record. The scan:

- validates the page-log header at the supplied log offset;
- walks complete records up to the current file size;
- ignores valid proof-only records;
- returns true for any complete non-proof record, including native-support,
  legacy, delta, sparse, or snapshot-boundary records;
- stops before an incomplete/corrupt tail record so interrupted appends do not
  become readable payload evidence;
- reports an error for invalid log headers or impossible offsets.

`database.cc` keeps uncheckpointed-record detection file-size based. Only the
payload/readable-record helper switches to the precise scan. If the scan
returns an error, the database helper fails closed and treats readable page
records as present.

## Affected Subsystems

- Ownerless page-log primitive API and primitive tests.
- Database ownerless ordinary-open and close-time retained-payload decisions.
- Ownerless compatibility and cross-process concurrency documentation.

## Compatibility Impact

No SQL, public `libmylite` C API, WAL format, or directory-layout behavior
changes. Proof-only records remain durable and retained by the existing
checkpoint rules; they simply stop enabling page-read hooks when no readable
page-version payload records exist.

## Database Directory And Native Storage Impact

No new files are introduced. The optimization only changes how existing
directory-owned `mylite-concurrency.wal` records are classified during open and
close. Native InnoDB files remain the authority when the WAL contains no
readable page-version records.

## Binary Size, License, And Dependencies

No dependency or license change. Binary-size impact is one small page-log scan
helper and focused primitive coverage.

## Test And Verification Plan

- Add primitive page-log coverage that:
  - reports no readable records for an initialized empty page log;
  - reports no readable records for proof-only-only WAL;
  - keeps reporting no readable records with an incomplete tail after
    proof-only records;
  - reports readable records after a complete non-proof page record is present.
- Run the ownerless primitive selector.
- Run focused ownerless history/native-support SQL selectors that depend on
  proof-only WAL behavior.
- Run the production performance probe in a reduced open/close shape to ensure
  the optimized build still emits comparable timing.
- Run production build guards, formatting, and whitespace checks.

## Acceptance Criteria

- Proof-only-only page logs do not count as readable page-version payload WAL.
- Mixed proof-only plus non-proof logs still count as readable page-version WAL.
- Incomplete tail bytes after proof-only records do not count as readable
  records.
- Uncheckpointed-record detection remains unchanged.
- Existing proof-only, replay, checkpoint, and focused SQL tests continue to
  pass.

## Risks And Unresolved Questions

- This does not shrink payload-bearing native-support records and does not
  solve broader write-path throughput.
- This does not address SQL-level table-lock fault injection, native
  redo/checkpoint reconciliation, DDL/file-lifecycle recovery, or external
  MariaDB/RQG stress.
- A future follow-up can add explicit open/close attribution for how often
  proof-only-only WAL avoids ordinary native read hooks in real workloads.
