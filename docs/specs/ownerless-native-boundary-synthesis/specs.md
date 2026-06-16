# Ownerless Native Boundary Synthesis

## Problem

Active page-version pins force close-time WAL retention so older snapshot
images remain available. Boundary records reduce the amount of state needed to
serve that snapshot and provide proof for later post-release cleanup, but the
product close path now avoids native checkpoint/reclaim while the pin is still
active.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` calls
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` before
  `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()` on the ownerless commit
  path. That gives MyLite a narrow point where the old native page image can
  still be read from the tablespace before the newer committed image is flushed.
- `mariadb/storage/innobase/buf/buf0flu.cc` copies dirty buffer-pool pages and
  calls `mylite_ownerless_innodb_publish_page_version()` with the commit-visible
  LSN; the actual flush path runs after publication.
- `packages/libmylite/src/database.cc` receives that page-publish hook and
  appends page-version WAL records through
  `mylite_ownerless_page_log_append_at()`.
- `packages/libmylite/src/ownerless_page_pin_registry.cc` can snapshot the
  active page-version pin count and the oldest pinned read LSN.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` already resolves an
  InnoDB tablespace by page-0 space id for no-live replay. The same resolver can
  read a native page only when its on-disk page LSN is at or below the oldest
  active snapshot LSN.

## Design

When a page-version publish happens while active snapshot pins exist and the
oldest pin is older than the page's commit-visible LSN, MyLite checks whether
the page-version WAL already contains a record for that page at or before the
oldest pin. If it does not, MyLite tries to read the current native tablespace
page at the same `space_id` and `page_no`.

The hook only attempts this synthesized boundary path for
snapshot-sensitive page images. InnoDB native-support state pages still publish
normal ownerless page-version records when the history/native checkpoint proof
requires them, but they do not require oldest-snapshot boundary records during
page-log checkpointing and now skip the auxiliary native-boundary probe. When
the shared page-version pin registry reports zero active pins, the helper also
returns before taking the registry latch.

The native page becomes a synthesized boundary record only if:

- the tablespace can be resolved uniquely by InnoDB page-0 space id,
- the requested page can be read at the expected page size,
- the page header still matches the requested `space_id` and `page_no`, and
- the native page LSN is non-zero and at or before the oldest active snapshot
  LSN.

Boundary synthesis is also suppressed for dictionary DDL statements and for
the post-DDL conservative-write window that follows local DDL while an external
page-version pin remains active. In that window, page LSN proves only the page
image age, not that a newly created file-per-table tablespace existed at the
older snapshot. The publish hook still records external-lineage consumption and
still publishes the current page-version record, so retained-reader WAL and
later conservative reclaim remain safe.

The paired post-DDL guard also suppresses external space-allocation refresh for
the same statement window. That keeps newly created tablespace allocation pages
from being refreshed from page-version or disk state selected under an older
retained snapshot while local post-create writes are still building the table.

The synthesized boundary record is appended to the ownerless page-version WAL
with `commit_lsn = oldest_snapshot_lsn`, then the normal newer page-version
record is appended. If any check fails, publication still proceeds without a
boundary record; later reclaim remains conservative and can leave the WAL
unchanged until the active pin releases.

## Scope And Non-Goals

In scope:

- Internal native-page read helper for ownerless tablespace replay paths.
- Page-publish-time boundary synthesis for active page-version pins.
- SQL coverage proving a live repeatable-read snapshot causes an old native
  boundary record to be retained while a peer writer commits.
- Boundary-synthesis pruning for no-active-pin publication and native-support
  state page classes.
- Boundary-synthesis suppression during dictionary DDL and the post-DDL
  conservative-write window, paired with external space-allocation refresh
  suppression for that same window.

Out of scope:

- Full DDL/file-lifecycle metadata for multi-file or ambiguous tablespace
  resolution.
- Background checkpoint pressure, user-visible checkpoint diagnostics, or group
  commit.
- Treating failed boundary synthesis as a commit failure.
- Replacing the ownerless history-proof/native-support page representation.

## Compatibility Impact

SQL results and isolation semantics are unchanged. The change gives active
snapshot readers a retained WAL boundary image earlier and reduces the state
needed after the pin releases.

## Directory And Lifecycle Impact

No new files or directory layout are introduced. Synthesized boundaries are
ordinary records in `concurrency/mylite-concurrency.wal` and are reclaimed by
the existing zero-active-pin or no-live checkpoint paths after they are no
longer needed by an active reader.

## Native Storage Impact

The native tablespace read is best-effort and read-only. It is accepted only
before the ownerless commit path flushes the newer dirty page image and only
when the page LSN proves it is visible to the oldest active snapshot.

## Test Plan

- Add primitive coverage for reading a native tablespace page at or before a
  target LSN.
- Add ownerless SQL coverage that starts from a checkpointed WAL, holds a
  repeatable-read snapshot, commits a peer update, verifies a WAL record at or
  before the snapshot LSN now exists, then releases the reader and verifies
  normal reclaim plus forced `.shm` rebuild preserve the committed data.
- Run embedded and hook ownerless primitive/cross-process SQL coverage, ownerless
  stress, `format-check`, and `git diff --check`.

## Acceptance Criteria

- Native page reads refuse newer pages and missing/ambiguous tablespaces.
- A writer committing while an older snapshot is active synthesizes at least one
  boundary record at or before the snapshot LSN.
- The active reader keeps seeing its old snapshot.
- Once the reader exits, normal close-time reclamation removes the retained
  boundary records and the final committed state survives reopen and forced
  shared-memory rebuild.

## Risks

- The current resolver is intentionally conservative and still depends on
  page-0 space-id discovery. Broader DDL/file-lifecycle metadata remains a
  separate ownerless gap.
- Page LSN is not DDL-created tablespace lifecycle proof. Local DDL and the
  following conservative-write window therefore suppress native boundary
  synthesis and external space-allocation refresh until the retained external
  pin releases.
- Boundary synthesis is opportunistic. If the native page has already advanced
  beyond the oldest snapshot LSN, MyLite keeps the existing safe behavior and
  retains the WAL until the active pin releases.
- Boundary-synthesis pruning does not reduce the remaining history-proof
  native-support page publication volume; that remains a separate write-path
  performance target.
