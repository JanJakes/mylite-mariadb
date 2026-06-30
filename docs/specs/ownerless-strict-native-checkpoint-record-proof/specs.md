# Ownerless Strict Native Checkpoint Record Proof

## Problem

No-live ownerless page-version WAL reclaim can discard records only when native
InnoDB files are authoritative for the reclaimed visible boundary. Existing
cutover proof verified the best retained record per `(space_id, page_no)`, but
that left the safety argument implicit for older retained records for the same
page: those records are safe to discard only if they have their own exact native
proof or are superseded by a later verified record for the same page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` commits page changes through
  mini-transaction redo before the page can become visible.
- `mariadb/storage/innobase/buf/buf0flu.cc` and
  `mariadb/storage/innobase/log/log0recv.cc` make the native checkpoint and
  recovery path authoritative for pages whose native LSN is covered.
- `packages/libmylite/src/database.cc`
  `ownerless_page_log_has_native_page_lsn_proof()` is the no-live reclaim gate
  before `mylite_ownerless_page_log_checkpoint_with_completion_at()` can compact
  retained page-version WAL.
- `packages/libmylite/src/database.cc`
  `verify_ownerless_native_page_checkpoint_latest_record()` supplies the exact
  byte-match, discarded-page, absent-space, and narrowly bounded native
  successor checks.

## Design

Tighten the native checkpoint proof gate:

1. Keep collecting retained proof records at or below the visible LSN while
   ignoring snapshot-boundary records.
2. Continue selecting and verifying the best record for each page identity.
3. Add a second pass across every collected retained record.
4. Accept a non-best record only when it either has its own exact/discard/absent
   native proof or is strictly superseded by the already verified record for the
   same `(space_id, page_no)`.
5. Fail closed and retain WAL if a retained record lacks that proof.

This preserves the existing no-live native successor path for current page
images while making the older-record discard argument explicit.

## Compatibility Impact

No SQL behavior, public API, file format, or directory layout changes. The
change may retain ownerless WAL longer when proof is incomplete; that is the
desired correctness-biased behavior.

## Database Directory And Lifecycle Impact

No new durable files. The focused test deletes `mylite-concurrency.shm` after
no-live reclaim has removed user-page WAL records. It deletes
`mylite-concurrency.wal` only when the WAL is fully checkpointed; native-support
rollback-history records may remain retained as the conservative existing
behavior.

## Native Storage Impact

No native InnoDB storage format changes. The implementation relies on existing
native page LSN/checkpoint checks and exact page-image comparison.

## Test Plan

- Add `duplicate-page-checkpoint-cutover`, which holds a live snapshot pin,
  applies two separate ownerless updates to the same InnoDB row, asserts
  duplicate retained user-page WAL identities exist, releases the pin, requires
  user-page WAL to drain to checkpointed or native-support-only state, removes
  `.shm`, and verifies ordinary native reopen sees the final committed rows.
- Run adjacent no-live/native checkpoint selectors:
  `no-live-native-checkpoint-cutover-proof`,
  `mixed-history-proof-checkpoint-cutover`, and
  `duplicate-page-checkpoint-cutover`.

## Acceptance Criteria

- The duplicate-page selector passes in a production embedded build.
- Existing native checkpoint cutover selectors continue passing.
- Docs and compatibility notes state that duplicate same-page retained WAL is
  discarded only with exact proof or a verified same-page successor.
