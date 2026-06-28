# Ownerless Active-Reader Pressure Killed Pin

## Problem Statement

Ownerless active-reader pressure coverage proves that live repeatable-read
snapshot pins retain page-version WAL across repeated writers and that normal
reader release allows later checkpoint reclamation. Existing killed snapshot
pin coverage proves dead-reader cleanup for a single retained update. The
remaining pressure crash gap needs a bounded case where a reader dies after
several retained pressure writes, while another ownerless process still has the
directory open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB/InnoDB repeatable-read transactions keep a stable read view while
  peer writers can commit newer versions.
- `mariadb/storage/innobase/read/read0read.cc` publishes and closes InnoDB read
  views through the MyLite ownerless hooks.
- `packages/libmylite/src/database.cc` registers ownerless page-version pins
  for repeatable-read and serializable transactions and uses those pins when
  enforcing `ownerless_page_log_limit_bytes`.
- `packages/libmylite/src/ownerless_page_pin_registry.cc` stores the
  directory-owned active pin state that live peers and no-live recovery use to
  distinguish live snapshot pins from dead process-owned pins.

## Design

Add a production SQL selector named `active-reader-pressure-killed-pin` and a
focused CTest named `libmylite.ownerless-active-reader-pressure-killed-pin`.

The selector:

1. Starts an idle ownerless live peer.
2. Starts a repeatable-read snapshot reader.
3. Runs a bounded sequence of ownerless writer opens; each writer commits one
   update and leaves page-version WAL retained by the reader pin.
4. Opens a configured ownerless writer with
   `ownerless_page_log_limit_bytes` equal to the retained WAL size and verifies
   a write returns `MYLITE_BUSY` while the reader pin is live.
5. Kills the snapshot reader.
6. Opens another configured ownerless writer while the idle peer remains live
   and verifies the same pressure limit no longer blocks a write after dead-pin
   cleanup.
7. Releases the live peer and verifies ownerless recovery checkpoints WAL,
   forced `.shm` rebuild preserves the final sum, and ordinary native reopen
   sees the same state.

## Scope And Non-Goals

In scope:

- Production ownerless SQL coverage.
- Multiple retained page-version WAL records under one live repeatable-read
  snapshot pin.
- A killed reader process while another ownerless peer remains live.
- Pressure-limit busy behavior before kill and successful retry after dead-pin
  cleanup.
- Ownerless, forced-`.shm`, and ordinary native reopen oracles.

Out of scope:

- Randomized active-reader pressure crash matrices.
- DDL worker crashes while pressure is active.
- Background checkpoint worker behavior.
- External MariaDB/RQG long-running stress.

## Compatibility Impact

No default SQL behavior changes. The selector strengthens evidence for the
existing ownerless pressure-limit policy: `MYLITE_BUSY` is returned only while
a live active snapshot pin retains WAL at or above the configured limit. Once
the pin owner is dead and cleanup runs, the same retained WAL size does not
block writes solely because a non-reading live peer remains open.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The test exercises existing
directory-owned process, read-view, page-pin, and page-version WAL state. It
keeps one live peer open to prove dead-reader cleanup is safe in live-ownerless
mode, then proves final no-live ownerless close can checkpoint the retained
WAL before native reopen.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The slice adds a
production test selector, CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run direct selector `active-reader-pressure-killed-pin`.
- Run the focused CTest
  `libmylite.ownerless-active-reader-pressure-killed-pin`.
- Run adjacent pressure selectors:
  `active-reader-pressure`, `active-reader-pressure-limit`,
  `active-reader-pressure-write-policy`, and `live-reclaim`.
- Run ownerless primitive/hook checks, pressure stress smoke, format check,
  CI production-build audit, and `git diff --check`.

## Acceptance Criteria

- Multiple writer commits remain visible while the live reader pins older WAL.
- A configured pressure-limit writer returns `MYLITE_BUSY` while the reader pin
  is live.
- Killing the reader lets a configured writer proceed while a separate
  ownerless peer is still open.
- The read-view active count reaches zero after dead-pin cleanup.
- Final ownerless close checkpoints retained WAL, forced `.shm` rebuild
  preserves the final sum, and ordinary native reopen observes the same state.

## Risks And Follow-Up

- This is a bounded killed-reader pressure crash case, not a broad
  active-reader pressure fuzz matrix.
- DDL pressure crash variants and longer external MariaDB/RQG-style stress
  remain planned.
