# Ownerless Native File-Op Marker Drain

## Problem

Ownerless dictionary DDL can persist a native file-operation checkpoint-needed
bit in `concurrency/mylite-concurrency.ckpt` when MyLite needs final native
`FILE_CHECKPOINT` evidence before later startup or page-log reclamation treats
native file operations as durable. The existing clear path lived inside
page-version WAL reclamation. If the checkpoint file had the marker but no
page-visible LSN, the final no-live close could force a native InnoDB
checkpoint and still leave the marker set because there was no WAL boundary to
compact.

That stale bit is conservative but harmful: later ownerless closes keep forcing
native checkpoints even after the checkpoint proof was already produced. The
slice also needs SQL-path evidence that a real `ALTER TABLE ... AUTO_INCREMENT`
statement marks the bit, keeps it while a live ownerless peer prevents final
drain, and clears it on the eventual final no-live close.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl()`
  persists the native file-op checkpoint-needed bit when a dictionary DDL
  needs checkpoint proof and the immediate checkpoint path does not clear the
  need.
- `packages/libmylite/src/database.cc::release_runtime()` reads that bit on
  final no-live ownerless shutdown and invokes
  `mylite_ownerless_innodb_make_checkpoint()`.
- `packages/libmylite/src/database.cc::reclaim_ownerless_page_log_after_native_checkpoint()`
  returned before reading or clearing the marker when `.ckpt` had no visible
  LSN.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_file()` durably
  flushes native file-operation redo before applying file renames or deletes.
- `mariadb/storage/innobase/fil/fil0fil.cc::fil_names_clear()` writes native
  `FILE_CHECKPOINT` evidence during a checkpoint.

## Scope And Non-Goals

In scope:

- Clear the native file-op checkpoint-needed bit after no-live ownerless close
  produces native checkpoint proof, even when no page-version visible LSN is
  present.
- Keep the existing page-version WAL reclamation path unchanged when a visible
  LSN exists.
- Add focused ownerless SQL harness coverage that seeds the marker with zero
  latest/visible checkpoint LSNs and verifies close drains it.
- Add focused ownerless SQL harness coverage that executes a real
  `ALTER TABLE ... AUTO_INCREMENT` statement, observes the marker before final
  no-live close, and verifies the final close drains it while preserving the
  table and next generated AUTO_INCREMENT value.

Out of scope:

- Broadening SQL DDL classes that set the marker beyond the already classified
  `ALTER TABLE ... AUTO_INCREMENT` case.
- Reconstructing missing DDL-created tablespaces from new metadata.
- Changing MariaDB redo formats, native file-operation redo parsing, or the
  ownerless page-version WAL format.

## Design

`reclaim_ownerless_page_log_after_native_checkpoint()` now handles the
zero-visible-LSN case before returning. For writable ownerless runtime with a
valid process slot, valid checkpoint/shared-memory files, and no live peers, it
reads the native file-op checkpoint-needed bit. If the bit is set, MyLite calls
`mylite_ownerless_innodb_make_checkpoint()` and clears the bit only after that
call succeeds.

The existing visible-LSN path still owns page-log compaction and still clears
the marker after native checkpoint preparation when WAL reclamation has a
checkpoint boundary to process.

## Compatibility Impact

No SQL syntax, public C API, or native file-format changes. The behavior change
is internal ownerless metadata cleanup: a final no-live close no longer carries
a stale native file-op checkpoint-needed bit after native checkpoint proof has
already been emitted.

## Directory And Lifecycle Impact

The slice changes only the contents of the existing
`concurrency/mylite-concurrency.ckpt` marker field. No new directory entries or
durable file formats are introduced.

## Native Storage Impact

Native InnoDB checkpointing still uses MariaDB's existing
`log_make_checkpoint()` path through the MyLite checkpoint hook. The marker is
cleared only after that hook reports success.

## Binary Size And Dependencies

No dependency changes. The production change is a small helper in
`database.cc`.

## Test Plan

- Add the `native-file-op-marker-drain` ownerless SQL selector.
- The selector initializes a normal database, verifies the ownerless WAL is
  checkpointed, writes zero latest/visible LSNs plus a native file-op marker
  into `.ckpt`, opens and closes ownerless read/write, and verifies the marker
  was cleared while the WAL remains checkpointed.
- Reopen with ordinary native read/write and verify the original table remains
  readable.
- In the same selector, create an InnoDB table with an AUTO_INCREMENT primary
  key, hold a live idle ownerless peer, execute
  `ALTER TABLE ... AUTO_INCREMENT = 100` through ownerless read/write, assert
  the marker is set while the live peer remains open, release the peer so the
  final no-live close drains the marker, force `.shm` rebuild, and verify a
  later native insert receives id 100.
- Run the focused selector, relevant checkpoint/reclaim selectors, ownerless
  SQL CTest shard coverage, ownerless hook subset, stress smoke, format check,
  and diff whitespace checks.

## Acceptance Criteria

- A marker with no page-visible LSN drains on final no-live ownerless close.
- `ALTER TABLE ... AUTO_INCREMENT` sets the native file-op checkpoint marker
  through the real SQL path, and final no-live close drains it.
- The marker is cleared only after `mylite_ownerless_innodb_make_checkpoint()`
  succeeds.
- Existing page-log reclamation and native exclusive reopen behavior remain
  unchanged.

## Risks

- The direct `.ckpt` seeding case still isolates the zero-visible-LSN metadata
  edge. The SQL case proves the existing AUTO_INCREMENT classifier, not a broad
  DDL marker matrix.
- Broader ownerless DDL/file-lifecycle recovery remains planned separately.
