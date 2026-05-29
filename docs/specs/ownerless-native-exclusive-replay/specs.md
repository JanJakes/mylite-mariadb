# Ownerless Native Exclusive Replay

## Problem

Ownerless writers can commit page histories through the MyLite page-version
WAL that are visible to ownerless readers before native InnoDB redo/checkpoint
state is fully reconciled for an ordinary exclusive `MYLITE_OPEN_READWRITE`
runtime. The previous safe ownerless reopen path could materialize page-version
records into native tablespace files, then truncate those records. A later
native exclusive open could still recover from an older process-local InnoDB
redo view and read stale pages because the page-version authority had already
been discarded.

## Source Findings

- MariaDB authority for this slice is the repository baseline
  `mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` runs
  `replay_concurrency_tablespaces()` during no-live shared-memory rebuild. That
  path applies visible page-version WAL images to native tablespace files before
  rebuilding volatile ownerless coordination.
- The same function previously called
  `mylite_ownerless_page_log_checkpoint_at(..., visible_lsn, ...)`, which
  truncated complete records at or below the durable ownerless visible LSN.
- `start_runtime()` maps ownerless coordination and installs InnoDB hooks for
  ordinary non-memory opens before `mysql_server_init()`, but page-version reads
  were enabled only for ownerless read/write or shared-read-only opens.
- MariaDB redo checkpoint discovery starts from redo-log checkpoint fields in
  `mariadb/storage/innobase/log/log0recv.cc`, while checkpoint writing in
  `mariadb/storage/innobase/buf/buf0flu.cc` assumes one process-local InnoDB
  redo/checkpoint owner. MyLite must not synthesize native checkpoint records
  without a separate MariaDB-source-backed reconciliation design.
- InnoDB page reads can overlay MyLite page-version WAL images through
  `mylite_ownerless_innodb_read_page_version()` when a page-visible LSN is
  active and page-log reads are enabled.

## Scope And Non-Goals

This slice is first-party MyLite runtime, primitive, test, and documentation
work. It does not edit MariaDB-derived source.

Non-goals:

- synthesizing native InnoDB checkpoint records;
- reclaiming retained page-version WAL after native exclusive reopen;
- changing DDL metadata routing or file-lifecycle replay coverage;
- changing public `libmylite` API or directory layout.

## Design

Keep the page-version WAL as the authoritative recovery view after no-live
tablespace replay until a later native checkpoint reconciliation slice can
prove it is safe to discard records.

Concretely:

- no-live tablespace replay still applies visible page images to native
  tablespace files;
- replay runs the page-log checkpoint path with safe LSN `0`, which preserves
  all complete committed page-version records while still trimming incomplete or
  corrupt tails and preserving the existing checkpoint fault-injection point;
- ordinary exclusive read/write opens enable page-version reads after mapping
  the directory-owned coordination state, so SQL reads can recover the latest
  ownerless-visible page image even if native InnoDB startup recovered from an
  older redo view;
- runtime attach reads the durable `.ckpt` redo/page-visible anchor and
  monotonically raises the mapped redo-state LSN fields before installing
  InnoDB hooks, so the first post-writer opener cannot choose a lower live
  read LSN from stale clean shared memory;
- ownerless read/write and shared-read-only behavior stays on the existing
  page-version read path.

This is a correctness hardening slice, not a WAL-reclamation slice. Native
InnoDB redo/checkpoint reconciliation and safe truncation of retained
page-version records remain planned work.

## Affected Subsystems

- Ownerless runtime startup and checkpoint attach.
- Ownerless redo-state primitive.
- Ownerless page-version WAL retention after no-live tablespace replay.
- InnoDB page-version read hook availability for ordinary exclusive read/write
  opens.

There is no wire-protocol, packaging, dependency, or binary-profile change
beyond the small first-party code added to `libmylite`.

## Compatibility Impact

`MYLITE_OPEN_READWRITE` no longer silently drops committed ownerless page
histories after multi-process ownerless commit shapes covered by the tests.
The ordinary exclusive runtime still holds the normal MyLite directory lock, so
there are no concurrent ownerless peers while the exclusive handle is open.

Database-directory impact is confined to retaining complete records in the
existing `concurrency/mylite-concurrency.wal` and reading the existing
`concurrency/mylite-concurrency.ckpt` anchor during runtime attach.

The cost is retained page-version WAL records after no-live replay. That is
intentional until MyLite can prove a native InnoDB checkpoint has incorporated
the ownerless page histories.

## Test Plan

- Add native exclusive `MYLITE_OPEN_READWRITE` total checks to the concurrent
  ownerless explicit-commit race before and after forced `.shm` rebuild.
- Add native exclusive total, version, and weighted-sum checks to the
  pseudo-random ownerless transaction stress case before and after forced
  `.shm` rebuild.
- Add primitive coverage proving checkpoint seeding raises redo-state LSNs
  monotonically without clearing live owner state.
- Run embedded ownerless SQL coverage, ownerless unsafe-hook coverage, and the
  ownerless stress preset.

## Acceptance Criteria

- Ownerless live reads still see concurrent committed writer output.
- Forced `.shm` rebuild still sees concurrent committed writer output.
- Native exclusive `MYLITE_OPEN_READWRITE` reopen sees the same committed
  output for the covered multi-writer explicit-commit shapes.
- Page-version WAL records needed for native exclusive page-version reads are
  retained after no-live tablespace replay.
- Clean shared-memory runtime attach cannot use a stale lower redo-visible LSN
  when `.ckpt` already contains a newer durable anchor.
