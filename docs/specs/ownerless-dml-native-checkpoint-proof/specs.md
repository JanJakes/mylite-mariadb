# Ownerless DML Native Checkpoint Proof

## Problem

Ownerless no-live reclaim currently drains DML-only native file-operation
markers for autocommit and single-owner explicit writers, but retains the same
marker plus page-version WAL when the writer was peer-observed or when the
closing runtime did not perform the local write. That is conservative and
correct, but it keeps committed DML page-version WAL as forced-`.shm` rebuild
evidence even after all peers have exited and native InnoDB pages can prove the
same durable image.

The next concurrency completion step is to replace this broad peer-explicit
DML-only retention with a native checkpoint proof for the no-live case. The
slice must not weaken active snapshot-pin handling or dictionary/file lifecycle
markers.

## Source Findings

- MariaDB base: MariaDB 11.8.6 (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`),
  as recorded in `docs/architecture/engineering-standards.md`.
- MariaDB InnoDB file-operation redo is logged from
  `mariadb/storage/innobase/fil/fil0fil.cc::mtr_t::log_file_op()`, including
  `FILE_MODIFY` records for tablespace page-size and size changes. MyLite's
  ownerless hook records that this redo class was observed through
  `mylite_ownerless_innodb_note_file_op_redo()`.
- `packages/libmylite/src/database.cc::
  mark_ownerless_native_file_op_checkpoint_after_successful_write()` marks the
  DML-only checkpoint record after representative DML statements when InnoDB
  observed file-operation redo outside dictionary DDL.
- `reclaim_ownerless_page_log_after_native_checkpoint()` reads both the generic
  native file-op marker and the DML-only marker. For no-live reclaim it already
  forces an InnoDB checkpoint when either marker is pending and calls
  `prepare_ownerless_page_log_native_checkpoint_for_reclaim()` before clearing
  markers and checkpointing page-version WAL.
- `prepare_ownerless_page_log_native_checkpoint_for_reclaim()` advances
  external LSN state, refreshes clean external pages unless safely skipped,
  requires `mylite_ownerless_innodb_checkpoint_covers_lsn(visible_lsn)`, then
  verifies page-version records through
  `ownerless_page_log_has_native_page_lsn_proof()`.
- `ownerless_page_log_has_native_page_lsn_proof()` collects the latest
  checkpointable user page-version record per `(space_id,page_no)`, ignores
  snapshot-boundary and native-support records, and requires either a matching
  native disk page image, a discarded/absent file-per-table page, or another
  explicit proof allowed by the caller. This is the proof needed for DML-only
  native-page reclamation after the last peer closes.
- `collect_ownerless_native_page_checkpoint_record()` carries the
  `MYLITE_OWNERLESS_PAGE_LOG_RECORD_EXTERNAL_SNAPSHOT_LINEAGE` flag into the
  native proof records. The no-live proof still requires native image,
  absence, or discard evidence before page-version WAL can be checkpointed.

## Design

- Remove the early no-live return that retains peer-explicit or no-local
  DML-only marker/WAL before native proof runs.
- Keep the existing guard conditions: the generic native file-op marker and
  autoincrement checkpoint marker still block this DML-only relaxation, and
  active snapshot pins still retain WAL through the existing page-version pin
  policy.
- Let no-live reclaim force the native checkpoint, advance page-visible state,
  run the existing native-page proof, then clear the DML marker and checkpoint
  the WAL only if proof succeeds.
- Preserve live-peer behavior. Live peers still retain user page-version WAL
  until the last peer exits because native-page proof is process-local and
  cannot update another process's clean native buffer-pool view.
- Preserve reader-only startup behavior. A runtime that only consumed retained
  WAL still cannot use a newer native page LSN as proof for non-external
  records, matching the existing `allow_no_live_consumed_native_successor`
  guard.

## Compatibility Impact

SQL results, public C API signatures, and native file formats are unchanged.
The compatibility claim narrows retained ownerless WAL after peer-observed
committed DML: after all peers close, MyLite may now drain the DML-only marker
and checkpoint page-version WAL when native files prove the visible boundary.

## Directory And Native Storage Impact

The MyLite database directory layout is unchanged. The slice reduces retained
`.wal`/`.ckpt` marker evidence only after native InnoDB pages inside the MyLite
directory prove the committed page images or safe absence/discard state.

## Non-Goals

- No change to generic dictionary/file lifecycle marker reclamation.
- No change to active-reader snapshot-boundary retention.
- No cross-process group commit claim.
- No randomized/RQG stress expansion in this slice.

## Test Plan

- Update the existing peer-explicit DML marker coverage so it proves:
  - marker/WAL retention while a peer is live,
  - DML marker drain after the final peer exits and no-live native proof runs,
  - ownerless reopen and ordinary native reopen after forced `.shm` rebuild
    still see the committed DML.
- Keep rollback/deadlock marker discard coverage unchanged.
- Run focused production selectors:
  - `native-dml-file-op-marker-drain`
  - `native-single-owner-explicit-dml-file-op-marker-drain`
  - `native-multi-peer-explicit-dml-file-op-marker-drain`
  - `native-explicit-dml-deadlock-file-op-marker-discard`
  - `commit-race`
  - `live-reclaim`
- Run the relevant ownerless CTest subset, production build guard,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Peer-explicit DML retains marker/WAL while another ownerless peer is live.
- After the final peer exits, no-live reclaim clears the DML-only marker and
  checkpoints the page-version WAL when native-page proof succeeds.
- Forced `.shm` rebuild followed by ownerless open and ordinary native open
  both preserve the committed DML row image without retained WAL overlay.
- Existing autocommit, single-owner explicit, rollback, deadlock, commit-race,
  and live-reclaim selectors continue to pass.

## Risks

- Some peer-explicit DML records may still fail proof if native disk state has
  not advanced enough by final close. In that case retention must remain the
  fallback; the slice must not clear markers before proof succeeds.
- External snapshot-lineage metadata is not a live-pin substitute. Final
  no-live DML reclaim may still checkpoint those records only after the same
  native proof succeeds; active readers remain protected by page-version pins.
