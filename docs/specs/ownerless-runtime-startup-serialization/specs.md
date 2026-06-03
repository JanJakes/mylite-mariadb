# Ownerless Runtime Startup Serialization

## Problem

Ownerless SQL tests open the same database directory from independent
processes. Focused generated-column and row-format DDL coverage exposed a
native InnoDB startup race: after one process initialized the embedded runtime,
a peer could enter MariaDB startup against the same directory before the first
open had completed connection and core `mysql.*` table bootstrap. The peer
occasionally aborted with invalid InnoDB redo-log header or missing checkpoint
evidence before any DDL-specific SQL ran.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::open_impl()` prepares the MyLite
  directory, starts the embedded MariaDB runtime, connects, creates core
  compatibility tables, and initializes the ownerless dictionary generation.
- `packages/libmylite/src/database.cc::start_runtime()` skipped the exclusive
  database-directory lock for ownerless runtime opens, mapped ownerless
  coordination state, installed InnoDB hooks, and serialized only
  `mysql_server_init()` with the existing system-table lock byte.
- `prepare_concurrency_metadata()` creates `concurrency/` and
  `mylite-concurrency.lock` inside `start_runtime()`. A broader startup lock
  that is acquired before `start_runtime()` must therefore create
  `concurrency/` first, otherwise the first ownerless opener after an ordinary
  exclusive initializer can fail before coordination metadata exists.
- `connect_runtime()` and `ensure_core_system_tables()` run after
  `start_runtime()` returns. Before this slice, a second process could acquire
  the same bootstrap lock and enter native InnoDB startup while the first
  ownerless open was still completing those phases.
- A native startup failure can leave MariaDB embedded process-global state
  behind even though `mysql_server_init()` returned an error. Retrying in the
  same process is only safe after the matching `mysql_server_end()` path runs.
- The existing `concurrency/mylite-concurrency.lock` byte-range scheme already
  reserves low lock bytes for persisted config, recovery, shared-memory resize,
  system-table bootstrap, dictionary statements, and global write statements.
- MyLite's portable byte-range lock helper uses classic POSIX `fcntl()`
  locks. Closing any descriptor for the same lock file can release other
  ranges held by the process, so a startup lock that shares
  `mylite-concurrency.lock` with nested bootstrap locks is not durable across
  the full open boundary.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_file()` writes and
  flushes native file-operation redo before `os_file_rename()` applies a DDL
  rename on disk.
- `mariadb/storage/innobase/log/log0recv.cc::recv_parse_or_apply_log_rec_body()`
  records `FILE_RENAME` recovery work only after recovery has seen a native
  `FILE_CHECKPOINT`; otherwise recovery can replay page records and file-create
  records without applying the already-completed final rename.
- `mariadb/storage/innobase/log/log0recv.cc::recv_recovery_from_checkpoint_start()`
  holds `log_sys.latch` while calling `buf_dblwr.recover()`. Ownerless
  uncheckpointed file-rename recovery can re-enter file-space lookup and wait
  while that latch is held, so ownerless startup must not retain the redo latch
  across doublewrite recovery in that mode.
- `mariadb/storage/innobase/buf/buf0flu.cc::fil_names_clear()` writes that
  `FILE_CHECKPOINT` during a native checkpoint. Ownerless background checkpoint
  suppression must therefore not block MyLite-owned no-live checkpoint evidence
  for completed native DDL file operations.

## Scope And Non-Goals

In scope:

- Serialize ownerless read/write and shared read-only runtime startup across
  the full `start_runtime()` / `connect_runtime()` /
  `ensure_core_system_tables()` / dictionary-generation initialization phase.
- Snapshot the MariaDB 10.8 redo startup prefix before ownerless native
  startup, restore the saved redo startup prefix after failed ownerless or
  native read/write startup attempts, and repair the redo prefix after final
  no-live ownerless native shutdown if `mysql_server_end()` leaves invalid
  startup checkpoint evidence in `ib_logfile0`.
- Use a distinct directory-owned lock file so this startup gate cannot be
  released by nested `mylite-concurrency.lock` bootstrap or system-table lock
  closes in the same process.
- Create the ownerless `concurrency/` directory before acquiring that startup
  lock so first ownerless opens do not depend on later metadata preparation.
- Retry a transient ownerless native startup failure a bounded number of times
  while holding the startup lock, after ending any partial MariaDB embedded
  initialization state.
- Let a final no-live ownerless read/write close publish a MyLite-owned native
  checkpoint before shutdown so completed DDL file operations have native
  `FILE_CHECKPOINT` evidence for later ownerless or native recovery.
- Treat ownerless dictionary DDL that changes `AUTO_INCREMENT` state as needing
  native checkpoint evidence before ownerless page-version WAL records can be
  safely reclaimed, even when no native file-rename redo callback fired.
- Recover relative ownerless file-operation redo names against the active
  MariaDB datadir, tolerate missing `FILE_CHECKPOINT` only when ownerless
  uncheckpointed file-operation recovery reaches a clean EOF boundary without
  corrupt filesystem evidence, replace stale rename targets during ownerless
  recovery, and avoid holding `log_sys.latch` across doublewrite recovery in
  that ownerless recovery mode.
- Keep normal non-ownerless exclusive read/write opens on the existing
  database-directory lock.

Out of scope:

- Changing MariaDB native redo formats or recovery semantics.
- Serializing all ownerless SQL execution after open.
- Replacing existing statement-level ownerless locks.

## Design

Add a private scoped byte-range lock helper for
`concurrency/mylite-runtime-startup.lock`. `open_impl()` now acquires a new
ownerless runtime startup lock after the database directory and `concurrency/`
directory exist and before `start_runtime()` is called for ownerless runtime
modes. The lock is held until `open_impl()` either returns a fully opened
handle or exits through an error path.

`open_impl()` creates `concurrency/` before acquiring the startup lock. The
metadata file, shared-memory file, page-version WAL, checkpoint file, and normal
`mylite-concurrency.lock` creation still remain in their existing preparation
paths after the broader startup boundary is held.

The startup lock uses a separate file from `mylite-concurrency.lock`. This
matters for classic `fcntl()` semantics: `start_runtime()` and
`ensure_core_system_tables()` still take their existing bootstrap locks on
`mylite-concurrency.lock`, and closing those descriptors must not release the
longer ownerless startup boundary.

Ownerless runtime opens make up to three `start_runtime()` attempts under the
same startup lock. A failed `mysql_server_init()` calls `mysql_server_end()`
before MyLite clears the partially prepared runtime state, so the retry does
not reuse stale MariaDB embedded process-global startup state. Non-ownerless
exclusive opens keep their single-attempt behavior.

Before `mysql_server_init()`, ownerless runtime startup snapshots the
`ib_logfile0` prefix only when the header and MariaDB 10.8 checkpoint pages
match the same acceptance rule that InnoDB startup uses. If startup finds a
checksum-valid header without usable checkpoint pages, MyLite restores the last
saved `mylite-redo-header.bin` startup prefix when that backup has valid
checkpoint pages and its recorded redo-file size differs from the current file
only within the bounded native redo-size tolerance. If startup fails after
MariaDB touched native state, MyLite ends the partial embedded runtime,
restores the saved 12 KiB startup prefix when available or the captured prefix
otherwise, and retries within the bounded startup-attempt limit. Ordinary
read/write native reopen uses the same failure-then-restore retry path so
native exclusive verification after ownerless activity can recover from a bad
startup prefix without rewriting redo during successful writer rounds.

Final no-live ownerless read/write shutdown also uses
`mylite-runtime-startup.lock`.
When the final closer is the only live ownerless process, it first invokes the
explicit MyLite InnoDB checkpoint hook. The hook bypasses ownerless checkpoint
suppression only for that calling thread, leaving background checkpoints
suppressed while allowing MariaDB to write the native `FILE_CHECKPOINT` marker
that recovery needs for completed DDL file-operation redo. The final closer then
holds the startup lock across close-time page-log reclaim, `mysql_thread_end()`,
`mysql_server_end()`, and redo-prefix repair, so no peer opener can enter
native startup while the final close is rewriting `ib_logfile0`. The closer
captures a valid redo prefix immediately before `mysql_server_end()` and, after
shutdown, restores the 12 KiB startup prefix containing the redo header page
and checkpoint pages.
When retained ownerless page-version WAL exists and active snapshot pins have
released, the no-live close-time reclaim path also forces a native checkpoint
even if the durable page-visible LSN already equals the latest ownerless LSN;
the native checkpoint proof is still required before those retained WAL records
can be truncated.
If a native file-operation checkpoint-needed bit is present but the checkpoint
file has no page-visible LSN to drive page-version WAL reclamation, final
no-live close still emits native checkpoint proof and clears the bit after that
checkpoint succeeds.

Successful ownerless dictionary DDL calls into the checkpoint marker path after
the dictionary-generation finish step. If InnoDB reported file-rename redo and
an immediate MyLite native checkpoint succeeds, no marker remains. Otherwise
MyLite persists a native file-operation checkpoint-needed bit in
`mylite-concurrency.ckpt`. `ALTER TABLE` statements containing
`AUTO_INCREMENT`, including high-watermark-only `ALTER TABLE ... AUTO_INCREMENT
= N`, set that marker because peer-visible ownerless page records are not
enough to prove native durability after the shared-memory file is rebuilt.

Ownerless recovery resolves relative FILE redo names against the active datadir,
records rename targets even before a native `FILE_CHECKPOINT`, and only
synthesizes the missing checkpoint boundary when ownerless uncheckpointed
file-operation recovery is enabled and recovery reaches a clean EOF boundary
without corrupt filesystem evidence. If redo replay finds a stale
tablespace already occupying the rename target, the ownerless path frees and
replaces that stale target. During this ownerless recovery mode, the redo latch
is released around `buf_dblwr.recover()` and reacquired before normal redo
recovery resumes, avoiding an embedded startup deadlock in doublewrite recovery.

The no-argument ownerless SQL aggregate harness now runs existing test cases by
`exec`ing hidden indexed children, and the database initializer itself runs in a
hidden `exec`ed child. Public selector names are unchanged, and the aggregate
path exercises the same post-`exec` process initialization boundary as focused
selector invocations without forking ownerless worker children from a process
that already started and stopped the embedded runtime.

## Compatibility Impact

No SQL syntax, public C API, or native file-format changes. The behavior change
is narrower concurrency during ownerless open: concurrent ownerless openers now
wait for the first opener to finish the native runtime startup boundary instead
of racing inside MariaDB/InnoDB initialization.

## Directory And Lifecycle Impact

The existing `concurrency/` directory gains `mylite-runtime-startup.lock`, a
private advisory lock anchor for ownerless runtime startup serialization. It
contains no application data and may remain after close like the other
directory-owned lock files.

## Native Storage Impact

Native InnoDB files remain in MariaDB format. The slice prevents a peer process from
starting native InnoDB against a database directory whose first ownerless opener
has not yet completed connection and compatibility-table initialization. It
also prevents a peer opener from racing a final ownerless shutdown window where
MariaDB embedded teardown can leave the redo startup prefix without valid
checkpoint evidence before the next ownerless startup reads it. A no-live final
close now allows an explicit native checkpoint before shutdown so copy-style DDL
rename redo has `FILE_CHECKPOINT` evidence before a later ownerless or native
startup replays InnoDB recovery.

## Binary Size And Dependencies

No dependency changes. The production change is limited to `database.cc` lock
coordination and a small scoped helper.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the embedded preset.
- Run repeated focused `generated-column-alter` ownerless DDL loops.
- Run repeated DDL-neighborhood coverage:
  `generated-column-alter`, `generated-column-index-ddl`,
  `charset-convert-ddl`, and `row-format-ddl`.
- Run focused `auto-inc-column-ddl` coverage, which exercises copy-style
  `ALTER TABLE ... ADD COLUMN ... AUTO_INCREMENT PRIMARY KEY FIRST`, ownerless
  peer metadata refresh, ownerless/native reopen, and forced `.shm` rebuild.
- Run focused `auto-inc-ddl` coverage, which exercises
  `ALTER TABLE ... AUTO_INCREMENT = N` high-watermark refresh, ownerless/native
  reopen, and forced `.shm` rebuild after native checkpoint evidence.
- Run the same DDL-neighborhood coverage in the unsafe hook preset, where the
  reduced ownerless page-log checkpoint threshold exposes redo/checkpoint
  startup races more aggressively.
- Run the embedded ownerless cross-process SQL aggregate CTest.
- Run the focused `commit-race` selector so first ownerless workers after an
  `exec`-isolated exclusive initializer cover bounded startup retry.
- Run repeated active-reader pressure stress while a live repeatable-read
  snapshot pin forces many ownerless writer open/close cycles against the same
  native directory.
- Rebuild and run ownerless hook and stress presets after the full slice.

## Acceptance Criteria

- Concurrent initial ownerless opens no longer abort with invalid native redo
  log header or missing checkpoint messages in the focused DDL loops.
- Hook-preset DDL-neighborhood loops no longer abort during peer opens with
  invalid native redo log header messages when no ownerless native write state
  is active.
- Ownerless stress active-reader pressure no longer aborts ownerless writer
  opens while a live reader keeps a repeatable-read snapshot pin active.
- Ownerless stress active-reader pressure checkpoints retained page-version
  WAL on the final no-live close after the repeatable-read snapshot pin
  releases.
- Focused commit-race coverage no longer aborts the first ownerless worker open
  after the isolated exclusive initializer with missing native checkpoint
  evidence.
- Focused `auto-inc-column-ddl` no longer leaves native recovery with a stale
  final table `.ibd` plus recreated `#sql-alter` tablespace after the copy-style
  ALTER's native file rename completed.
- Focused `auto-inc-ddl` remains durable through ownerless reopen, native
  exclusive reopen, forced `.shm` rebuild, and another native exclusive reopen
  after the high-watermark-only ALTER.
- The full embedded ownerless cross-process SQL aggregate passes.
- Ownerless execution after open remains governed by the existing ownerless
  statement, transaction, record, page-write, redo, and dictionary locks.

## Risks And Follow-Up

- This fixes startup serialization and no-live final-close checkpoint evidence
  for completed native DDL file operations; broader live-peer DDL/file
  lifecycle crash recovery remains a separate gap.
- The open path is intentionally more serialized than steady-state ownerless
  SQL execution.
