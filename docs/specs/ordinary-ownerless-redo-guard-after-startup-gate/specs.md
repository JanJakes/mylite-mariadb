# Ordinary Ownerless Redo Guard After Startup Gate

## Problem

CI intermittently failed `libmylite.embedded-ownerless-directory-lifecycle`
after the embedded performance probes with:

```text
InnoDB: Invalid log header checksum
InnoDB: Plugin initialization aborted with error Data structure corruption
Unknown/unsupported storage engine: InnoDB
```

The failure happened in `test_concurrency_shared_memory_is_grow_only()` when a
fresh database was created and closed through an ordinary read/write open, then
ownerless metadata was introduced before the next ordinary open. The startup
gate correctly avoids ownerless coordination setup for fresh ordinary opens,
but its close-side narrowing also skipped the MariaDB redo-prefix repair for
ordinary runtimes that had not mapped ownerless files.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0recv.cc:1736` reports
  `Invalid log header checksum` when startup checkpoint discovery cannot
  validate the redo header block.
- `docs/specs/embedded-repeated-open-redo-repair/specs.md` already specifies
  the intended repair: capture a valid native redo startup prefix before
  `mysql_server_end()` and conditionally restore it after shutdown for any
  non-read-only, non-memory final runtime close.
- `packages/libmylite/src/database.cc:release_runtime()` had been narrowed to
  run that repair only when ownerless coordination files were mapped. That kept
  ordinary startup fast, but it also meant an ordinary create/close could leave
  the next ownerless-recovery open exposed to the intermittent MariaDB shutdown
  redo-header state.

## Design

Keep the ownerless startup gate unchanged: fresh ordinary opens still do not
create or map `concurrency/` files.

Broaden only the shutdown redo-prefix repair candidate to all final
non-read-only, non-memory runtime closes. Ordinary closes hold `mylite.lock`, so
no peer can be modifying the database while the process-local prefix snapshot is
captured and conditionally restored. Ownerless closes keep their existing
startup-lock and no-live-peer restore rules.

The repair remains lightweight:

- it captures only an in-memory prefix that MariaDB-current checkpoint
  validation accepts before shutdown;
- it skips the write when the post-shutdown prefix is already valid;
- it creates no durable sidecar for fresh ordinary databases;
- it does not create or map ownerless WAL, checkpoint, or shared-memory files.

## Compatibility Impact

No SQL, C API, PHP API, or directory-format behavior changes. Fresh ordinary
databases still avoid ownerless coordination files, and ordinary opens after
ownerless evidence still use the existing recovery path.

## Native Storage Impact

The existing native `datadir/ib_logfile0` startup prefix may be restored after
embedded shutdown only when MariaDB left a prefix that no longer validates and
the pre-shutdown prefix did validate. This is the same native restartability
guard as the repeated-open repair; the slice restores the intended ordinary
coverage after the startup-gate performance optimization.

## Test Plan

- Strengthen `test_concurrency_shared_memory_is_grow_only()` so the ordinary
  pre-ownerless close creates and writes an InnoDB table before ownerless
  metadata is introduced.
- Run focused production lifecycle selectors:
  `libmylite.embedded-open-close` and
  `libmylite.embedded-ownerless-directory-lifecycle`.
- Run repeated focused lifecycle selectors to increase coverage for the
  intermittent redo-header failure.
- Run the minimal production performance probe to confirm ordinary startup
  still avoids ownerless setup and to record the close-side redo guard cost.
- Run production build guards, `format-check`, and `git diff --check`.

## Acceptance Criteria

- Ordinary startup still avoids ownerless SHM/WAL/checkpoint setup when no
  durable ownerless evidence exists.
- A database created and closed through an ordinary InnoDB runtime can later
  introduce ownerless metadata and reopen successfully.
- Ordinary close-time redo guard remains cheap relative to MariaDB
  `mysql_server_end()` and does not create ownerless coordination files.
- CI no longer fails the embedded ownerless directory lifecycle selector with
  the repeated-open redo-header checksum signature.

## Risks And Follow-Up

This does not complete broader ownerless native redo/checkpoint reconciliation.
It only restores the native redo startup-prefix guard on ordinary shutdowns so
the startup gate cannot regress restartability before ownerless recovery has a
chance to run.
