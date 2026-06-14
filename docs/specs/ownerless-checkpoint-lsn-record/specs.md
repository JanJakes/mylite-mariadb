# Ownerless Checkpoint LSN Record

## Problem

`mylite-concurrency.ckpt` persists the ownerless raw-latest and page-visible
LSN pair used to seed rebuilt shared redo state and ordinary native refresh.
Before this slice, the durable pair was a single 16-byte payload with no
generation or checksum. A torn or stale pair could not be distinguished from a
complete update by local checkpoint readers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` owns
  `mylite-concurrency.ckpt` creation, checkpoint LSN publication, and checkpoint
  LSN reads through `prepare_concurrency_checkpoint_file()`,
  `write_concurrency_checkpoint_lsn_locked()`, and
  `read_concurrency_checkpoint_lsn()`.
- Existing checkpoint writes are protected by the `CHECKPOINT` byte-range lock
  and optionally call `sync_fd_data()` for durable publication.
- Existing checkpoint reads are used during ownerless runtime startup, `.shm`
  rebuild, ordinary native read/write reopen, baseline snapshot seeding, and
  no-live native checkpoint reclamation.
- MariaDB's CRC32C implementation is already linked into the embedded profile
  and used by MyLite page-log payload checksums and InnoDB checksum paths.

## Scope And Non-Goals

In scope:

- Append two fixed-size checkpoint LSN records after the existing checkpoint
  payload.
- Store a generation, latest LSN, visible LSN, and CRC32C in each record.
- Alternate writes between the two records so a torn latest record can fall
  back to the previous valid generation.
- Preserve the legacy 16-byte pair for existing files and compatibility with
  older tooling.
- Prove a corrupted latest record is ignored while the previous valid record
  remains usable and can be repaired by a later ownerless write.

Out of scope:

- Changing the checkpoint recovery header format or database UUID binding.
- Moving the native file-operation checkpoint-needed marker into the LSN record;
  a follow-up slice adds separate checksummed marker records instead.
- Solving the broader native redo/checkpoint reconciliation and DDL
  file-lifecycle protocol.
- Replacing page-version WAL replay with native redo.

## Design

The checkpoint file keeps its existing recovery header and legacy fields:

- latest LSN at offset 128,
- visible LSN at offset 136,
- native file-operation checkpoint marker at offset 144.

Two 64-byte LSN records now start at offset 152. Each record stores:

- magic `MYLCLSN1`,
- format `1`,
- generation,
- latest LSN,
- visible LSN,
- CRC32C over the bytes before the checksum field,
- zeroed reserved tail bytes.

Checkpoint writes still run under the existing checkpoint byte-range lock. A
write reads the current highest valid generation, writes the next generation to
the alternating slot, updates the legacy 16-byte pair, and then performs the
existing durable sync when requested.

Checkpoint reads prefer the highest valid checksummed generation. If both
record slots are empty, readers fall back to the legacy pair. If any slot is
non-empty but no valid slot exists, the read fails closed instead of trusting a
possibly torn legacy pair. If one slot is corrupted and the other is valid, the
valid slot is used.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, or wire-protocol change. Existing checkpoint
files without LSN records remain readable because empty trailer slots fall back
to the legacy pair. New files remain self-contained in the MyLite database
directory.

## Directory And Lifecycle Impact

`mylite-concurrency.ckpt` grows from the legacy payload end at offset 152 to
include two 64-byte LSN record slots. No new file is introduced.

## Native Storage Impact

No native InnoDB file or redo format changes. The record strengthens the MyLite
checkpoint evidence used to decide when native pages and retained page-version
WAL are safe to use.

## Build, Size, License, And Dependencies

No new dependency or license impact. The implementation reuses MariaDB's
already-linked `my_crc32c()`.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused SQL case
  `test_ownerless_checkpoint_lsn_record_recovers_from_torn_latest_slot`.
- Run nearby checkpoint selectors, including native checkpoint reclamation and
  visible/checkpoint crash coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- A checkpoint write publishes a valid checksummed LSN record and keeps the
  legacy latest/visible pair current.
- Runtime checkpoint reads prefer the highest valid generation.
- A corrupted latest-generation record does not prevent reopen while the
  previous generation is valid.
- A later ownerless write repairs the corrupted slot with a new valid
  generation.
- Existing checkpoint files with empty record slots still read through the
  legacy pair.

## Implementation Evidence

- `write_concurrency_checkpoint_lsn_locked()` now writes alternating
  checksummed LSN records before updating the legacy latest/visible pair.
- `read_concurrency_checkpoint_lsn()` now chooses the highest valid record,
  falls back only when both slots are empty, and fails closed when non-empty
  slots contain no valid record.
- `test_ownerless_checkpoint_lsn_record_recovers_from_torn_latest_slot()`
  corrupts the latest generated record, zeros the legacy pair, forces `.shm`
  rebuild, verifies ownerless reopen can read committed data through the
  previous valid record, and verifies a later write repairs the record set.
- Production `mylite_ownerless_cross_process_sql_test` build passed.
- Focused direct `sql-case` coverage passed for:
  `test_ownerless_checkpoint_lsn_record_recovers_from_torn_latest_slot`,
  `test_ownerless_native_checkpoint_reclaims_page_log`,
  `test_ownerless_native_file_op_marker_clears_without_page_log`,
  `test_ownerless_native_file_op_marker_drains_after_real_sql_ddl`, and
  `test_rebuild_checkpoints_committed_page_versions`.
- Direct `visible-checkpoint-crash` harness coverage passed.
- `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check` passed.

## Risks And Follow-Up

- The native file-operation checkpoint-needed marker remains separate from the
  LSN record by design; the follow-up ownerless native file-op marker record
  slice adds its own checksummed generation records with conservative
  checkpoint-needed recovery.
- Broader native redo/checkpoint reconciliation remains partial until DDL
  file-operation recovery, native checkpoint proof, page-version compaction,
  and external stress all cover the remaining lifecycle shapes.
