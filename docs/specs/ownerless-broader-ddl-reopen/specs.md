# Ownerless Broader DDL Reopen

## Problem

The broader ownerless DDL selector now proves an already-open peer can refresh
foreign-key, generated-column, online-index, column-shape, `CREATE TABLE ...
LIKE`, CTAS, and explicit instant-column metadata while another ownerless
process performs the DDL. That is live-peer evidence. The DDL/file-lifecycle
recovery gap also needs no-live reopen evidence: after every ownerless process
exits, the same DDL-created and altered native InnoDB tables must be readable
through ownerless read/write open, ordinary exclusive read/write open, and a
forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` runs no-live ownerless recovery by
  rebuilding stale volatile coordination, replaying retained page-version WAL
  into existing native tablespace files, and seeding shared redo/page-visible
  checkpoint state.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` resolves existing
  InnoDB tablespaces by native page-0 space id and can product-mode skip
  retained records for tablespaces that no longer exist.
- `test_ownerless_broader_ddl_refreshes_peer_dictionary()` already keeps a peer
  open while a child performs representative DDL and validates the live
  dictionary refresh boundary after each child signal.
- `remove_concurrency_shm()` is already used by nearby ownerless SQL tests to
  force `.shm` recreation and prove durable state does not depend on volatile
  mapped memory.

## Scope And Non-Goals

- Add final-state reopen assertions to the broader DDL selector after both live
  ownerless handles close.
- Verify the final broader DDL state through ownerless and ordinary exclusive
  read/write opens before and after deleting `concurrency/mylite-concurrency.shm`.
- Do not change no-live replay algorithms, InnoDB dictionary/file creation, or
  native page-version truncation policy.
- Do not claim full DDL-created tablespace recovery. This remains evidence for
  existing native files and the representative broader DDL selector.

## Design

- Factor the final broader DDL state checks into a helper that accepts
  `open_database_paths` plus open flags.
- Assert the foreign-key, generated-column, altered online table,
  `CREATE TABLE ... LIKE` copy, CTAS copy, and explicit instant-column table all
  retain the expected rows and metadata.
- After the live peer closes and the DDL child exits, run the helper with:
  - `MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW`,
  - `MYLITE_OPEN_READWRITE`,
  - forced `.shm` deletion followed by ownerless read/write, and
  - ordinary exclusive read/write after the forced rebuild.

## Compatibility Impact

No SQL behavior changes. The compatibility matrix gains stronger evidence that
ownerless broader DDL output survives no-live recovery and ordinary native
exclusive reopen, including after volatile shared-memory recreation.

## Directory And Lifecycle Impact

The slice only exercises existing files inside the MyLite-owned database
directory: native InnoDB table files under `datadir/`, ownerless WAL/checkpoint
anchors under `concurrency/`, and the volatile shared-memory file that is
deleted during the forced-rebuild assertion.

## Native Storage Impact

No native storage format changes. The test validates that MariaDB's native
InnoDB metadata and MyLite's retained page-version/recovery anchors leave the
broader DDL final state usable after ownerless peers have gone away.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `ddl-broader` selector.
- Run the focused `ddl-broader` selector in the unsafe-hook preset.
- Run `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`.
- Run the ownerless stress preset.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Broader DDL final state survives ownerless reopen after all peers exit.
- Broader DDL final state survives ordinary exclusive read/write reopen.
- Forced `.shm` rebuild does not lose the broader DDL final state.
- The compatibility docs keep broader DDL/file-lifecycle recovery marked
  partial rather than complete.

## Risks And Follow-Up

- This does not synthesize missing `.ibd` files or durable DDL file-lifecycle
  metadata. Product no-live replay still skips retained page-version records for
  tablespaces that no longer exist.
- External MariaDB/RQG DDL stress remains planned.
