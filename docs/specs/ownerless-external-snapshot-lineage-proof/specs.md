# Ownerless External Snapshot Lineage Proof

## Problem

After a repeatable-read ownerless reader releases its page-version pin, a writer
runtime can still hold page-version WAL that was originally retained for that
external snapshot. Native checkpoint proof was too strict for file-per-table
pages whose on-disk image had already advanced to a newer LSN inside the same
post-release writer runtime, so active-reader pressure could leave
`concurrency/mylite-concurrency.wal` non-empty after the final close.

A broad no-live rule that accepted any newer native page was unsafe: the
commit-race test needs unmarked concurrent-writer WAL to remain replayable
until exact native proof exists.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` publishes ownerless modified page
  images before commit-time dirty-page flush.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` exposes
  native page LSN and disk-image comparison hooks used by MyLite checkpoint
  proof.
- `packages/libmylite/src/database.cc` tracks ownerless page-version pins in
  the shared page-pin registry and performs close-time native checkpoint proof
  before truncating the page-version WAL.
- `packages/libmylite/src/ownerless_page_log.cc` rewrites delta-encoded
  retained records during checkpoint, so record metadata must be preserved
  across direct appends and checkpoint rewrite.

## Design

Add an ownerless page-log metadata bit named external snapshot lineage. MyLite
sets this bit on normal page-version records when either:

- the publish happens while another owner has an active page-version snapshot
  pin, or
- the runtime opened or consumed existing page-version WAL while another owner
  had an active page-version snapshot pin.

Native checkpoint proof keeps exact matching as the default. It accepts a newer
native file-per-table page as proof only when all of these are true:

- no live ownerless peers remain,
- the runtime consumed page-version WAL,
- the retained record has the external snapshot lineage bit,
- the record is not a synthesized snapshot-boundary record,
- the record is for a user file-per-table page, and
- the native disk page LSN is greater than the record page LSN and no greater
  than the visible reclaim LSN.

This makes the active-reader post-release catch-up path reclaimable without
weakening commit-race records that were produced by concurrent writers rather
than by an external snapshot lineage.

## Compatibility Impact

SQL behavior and isolation semantics are unchanged. The change narrows internal
native checkpoint proof so WAL retained for an external reader can be reclaimed
after that reader releases, while unrelated concurrent-writer WAL still
requires exact native proof or replay.

## Directory And Lifecycle Impact

No new files or directory layout changes. The lineage marker is an internal
record flag in `concurrency/mylite-concurrency.wal` and is preserved when
checkpoint rewrites retained delta records.

## Native Storage Impact

No native file format changes. The newer-native-page acceptance is limited to
proved file-per-table pages and only after no live peer can need the older
snapshot image.

## Test Plan

- Primitive page-log test proving the external snapshot lineage bit is written,
  queryable, and preserved when a delta record is rewritten during checkpoint.
- Focused active-reader pressure SQL case:
  `mylite_ownerless_cross_process_sql_test sql-case
  test_ownerless_active_reader_pressure_limit_blocks_write_classes`.
- Focused commit-race regression:
  `mylite_ownerless_cross_process_sql_test commit-race`.
- Adjacent ownerless pressure/native reclaim selectors, hook ownerless coverage,
  ownerless stress, format, and diff checks before commit.

## Acceptance Criteria

- Active-reader pressure checkpoints the page-version WAL after reader release
  and final close.
- Commit-race still preserves every worker commit before and after forced
  shared-memory rebuild.
- Unmarked records do not receive the newer-native-page proof relaxation.
- Page-log metadata survives checkpoint rewrite.

## Risks And Follow-Up

- This is not broad DDL/file-lifecycle recovery. Missing or ambiguous native
  file identity still keeps the safe retained-WAL behavior.
- SQL-level table-lock fault injection remains unproven because previously
  explored SQL forms stopped before the ownerless table-wait callback.
- External MariaDB/RQG and larger active-reader pressure policies remain
  separate evidence tracks.
