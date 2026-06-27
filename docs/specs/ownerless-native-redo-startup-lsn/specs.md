# Ownerless Native Redo Startup LSN And Transaction Images

## Problem

Ownerless no-live recovery can checkpoint page-version WAL and leave native
InnoDB files as the authoritative recovery source. A reproduced hook selector,
`test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds`,
can then reopen with an empty ownerless WAL and a nonzero ownerless checkpoint
visible LSN while an InnoDB undo tablespace page has a page LSN ahead of the
native redo log's current flushed LSN. MariaDB aborts startup with the normal
"page log sequence number is in the future" corruption guard before MyLite can
refresh the ownerless native boundary.

After that startup abort is avoided, the same selector can expose a committed
row update as `SUM(value)=30` instead of `31`: the holder transaction has a
captured user page image, but resident-buffer validation rejects same-LSN images
whose only difference is flush checksum/header normalization, leaving only
native-support proof records before a killed waiter is cleaned up. The same
record-lock-grant crash class can also leave a different-LSN resident buffer
page around the holder's commit boundary; that page is not proof that the
holder's bounded committed transaction image is invalid.

After the committed image is retained in the page-version WAL, a fresh
ownerless opener can rebuild stale shared memory left by a killed waiter. The
stale-reader rebuild shortcut was allowed to discard WAL without materializing
tablespaces; that is correct only for reader-boundary/native-support records,
not for committed user page images. The same selector can also read the
retained image successfully and still leave WAL uncheckpointed on close because
the running InnoDB buffer pool has not made the native file prove the retained
same-LSN user image before no-live checkpoint proof.

The slice is limited to that startup and transaction-image boundary. It does not
change ownerless WAL format, page-version replay, redo-header sidecar format, or
MariaDB's corruption diagnostics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), as recorded in
  `docs/architecture/engineering-standards.md`.
- `mariadb/storage/innobase/buf/buf0buf.cc` checks page LSNs in
  `buf_page_check_lsn()`. Before reporting a future page LSN, the MyLite fork
  tries `mylite_ownerless_innodb_advance_external_lsn(page_lsn)`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` only
  advances the external LSN when ownerless InnoDB lock hooks are installed:
  `mylite_ownerless_innodb_advance_external_lsn()` returns unavailable without
  `mylite_ownerless_innodb_lock_has_hooks()`.
- `packages/libmylite/src/database.cc` currently treats no-live ownerless
  startup with empty ownerless WAL as "current native redo is authoritative" and
  may skip installing InnoDB lock hooks before `mysql_server_init()`.
- The same startup path already computes
  `ownerless_startup_native_purge_drain_needed` when the authoritative native
  path has a nonzero ownerless checkpoint visible LSN, and later performs native
  checkpoint refresh after MariaDB starts.
- The native file-op and DML checkpoint markers are read after the initial
  authoritative-native decision. A pending DML marker is evidence that native
  DML still needs ownerless reconciliation; it must not be used to skip ownerless
  hooks, even though the current native redo header is still the startup source.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  validates captured transaction page images against the resident buffer-pool
  page before publishing them. A real resident-page mismatch must remain a
  rejection, but a same-LSN page that differs only because InnoDB initialized
  flush checksum fields is still the same committed page image.
- `packages/libmylite/src/database.cc` treats some stale-reader shared-memory
  rebuilds as safe to discard page-version WAL. That shortcut must distinguish
  native-support/proof records from committed user page images before it drops
  retained WAL.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` already rewrites
  same-LSN different page images in generic replay. Product startup replay needs
  the equal-LSN native-page preservation rule only for snapshot-boundary
  records, not for ordinary committed user images.

## Design

Enable a temporary startup-only LSN-advance gate during `mysql_server_init()`
when the runtime is ownerless read/write, no-live, WAL-empty, and the ownerless
checkpoint visible boundary is nonzero. The gate is capped at that persisted
ownerless visible LSN. This keeps MariaDB's page-LSN validation strict while
allowing the existing MyLite `advance_external_lsn()` hook to make the native
redo LSN catch up only to pages that MyLite already made authoritative through
the ownerless checkpoint.

Split the startup decision between "the current native redo header is the
startup source" and "native state is reconciled enough to skip ownerless hooks".
With a pending native file-op or DML checkpoint marker, the current native redo
header is still preserved and a stale sidecar redo backup is not restored, but
full ownerless hooks remain installed so the post-start native checkpoint
refresh can seed the ownerless boundary. Without pending markers, the normal
no-live authoritative-native fast startup remains available. Full ownerless lock
hooks are deliberately not installed for the pre-hook purge-drain window because
`mylite_ownerless_innodb_settle_purge_before_hooks()` requires lock hooks to be
absent before it drains native purge state.

When validating a captured transaction image against a resident same-LSN page,
first compare the raw captured bytes. If they differ but the page LSN matches,
normalize a copy of the captured image with the same flush checksum/header
initialization used for page-version publication and accept it only if the
normalized copy matches the resident page byte-for-byte. Same-LSN resident
pages that still differ after normalization reject the captured image and keep
the existing conservative fallback.

Different resident page LSNs are not treated as image corruption by themselves.
If the captured transaction image's page LSN is at or below the transaction's
visible commit boundary, publish the image with that visible boundary even when
the current buffer page has moved to another LSN. Future images above the
transaction visible boundary are still rejected.

Shared-memory rebuild now scans retained WAL before using the stale-reader
discard path. If only native-support/proof records are present, the existing
discard remains valid. If a committed user page image is present, rebuild first
materializes visible tablespace pages and keeps the WAL as the authoritative
ownerless recovery source until a later no-live close can checkpoint it.

Product tablespace replay preserves an equal-LSN native page only for records
explicitly marked as snapshot boundaries. Ordinary committed user images rewrite
same-LSN different native pages, matching the generic replay primitive and
preventing a committed retained image from being skipped as if it were only a
reader boundary.

On final no-live shutdown, if retained ownerless WAL payload still exists after
MariaDB stops, MyLite replays visible tablespace images once the InnoDB buffer
pool can no longer overwrite the files, then checkpoints the WAL at the
ownerless visible LSN. This is deliberately a shutdown fallback for retained
payload; it does not replace normal live-peer retention or running-engine native
checkpoint proof.

When no-live close forces native checkpoint proof, MyLite records the newest
ownerless latest LSN separately from the visible boundary and clamps the visible
boundary to MariaDB's exact native checkpoint LSN. It no longer treats the
checkpoint-record-size coverage gap as sufficient to advertise a newer visible
boundary before WAL-empty native-authoritative startup; startup can raise the
visible boundary after MariaDB has recovered normally.

## Compatibility Impact

This preserves MariaDB's native recovery semantics. MyLite is not accepting
future pages globally; it only lets ownerless startup expose a durable ownerless
checkpoint boundary to the existing InnoDB page validation hook. Transaction
image validation remains strict: only same-LSN checksum-normalized equivalence
is accepted as a same-LSN match, and different-LSN resident pages only permit
bounded committed transaction images whose page LSN is covered by the
transaction visible LSN.

SQL behavior and public C API behavior are unchanged. The effect is that
ownerless crash-recovery opens that previously failed with an InnoDB startup
corruption diagnostic, returned a stale committed row through stale-reader
rebuild, or left retained WAL uncheckpointed on final no-live close can recover
through the documented ownerless native checkpoint boundary.

## Storage And Lifecycle Impact

No durable file names or formats change. The slice only adds a transient
startup gate inside the existing ownerless InnoDB hook module and tightens the
in-memory startup hook decision around pending native markers. The shared-memory
rebuild path now classifies retained WAL before choosing discard versus
tablespace replay, and final no-live shutdown may replay retained visible
tablespace images after `mysql_server_end()` before checkpointing WAL. The
embedded lifecycle enables the gate immediately before `mysql_server_init()` and
clears it immediately after startup returns, including failure paths.

## Tests

- Reproduce and fix the hook selector
  `test_crashed_record_lock_grant_blocks_peer_cleanup_until_reopen_rebuilds`.
- Verify the selector preserves the holder's committed `+1` through ownerless
  and ordinary native reopen while the killed waiter remains uncommitted.
- Verify stale-reader shared-memory rebuild does not discard committed user
  image WAL and final no-live close checkpoints retained visible WAL after
  post-shutdown tablespace replay.
- Rerun the adjacent production commit-race selector because it has shown the
  same future-LSN and native redo replay abort classes intermittently.
- Run focused ownerless primitive/hook CTests and the active-writer visible-skip
  regression from the previous slice.
- Run formatting and whitespace checks before commit.

## Acceptance Criteria

- The hook record-lock-grant crash selector opens successfully and verifies the
  committed sum through ownerless and native reopen.
- The previous active-writer visible-skip regression remains green.
- Focused production/hook verification and format checks pass, or any remaining
  unrelated intermittent failure is documented with an isolated passing rerun.

## Risks

The startup gate must remain narrower than full ownerless hooks; otherwise the
native purge-drain helper can refuse to run and recovery opens can return busy
after passing page validation.

Post-shutdown replay is intentionally no-live-only. Running-engine direct file
replay would risk racing MariaDB's buffer pool and is not part of this slice.
