# Append Page Buffer Flush Window

## Problem

The active append write buffer batches contiguous unpublished page runs, but its
256-page flush window still forces frequent `pwrite()` calls during large
row-DML transactions. Local storage-smoke profiling after the row and exact
index cache work still shows prepared update time dominated by
`flush_statement_append_page_buffer()` writing the buffered append run.

This slice widens the transient flush window without changing the durable page
format or checkpoint lifecycle.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc:3193-3266` drives handler savepoint set, rollback,
  and release callbacks. The MyLite buffer must therefore keep nested
  statement rollback deterministic.
- `mariadb/storage/mylite/ha_mylite.cc:2354-2377` opens nested MyLite
  checkpoints for row DML inside SQL transactions, so a top-level transaction
  can accumulate successful statement appends before commit.
- `packages/mylite-storage/src/storage.c` owns the append buffer in the
  outermost active checkpoint, serves buffered pages through `read_page_at()`,
  flushes before top-level header publication, and flushes retained prefixes
  before rollback truncation.
- A 300k-update local sample with a 2048-page window still spent meaningful time
  in `pwrite()`, but the hot call count was much lower than the previous
  256-page profile. The remaining cost is physical append volume, not only
  syscall count.
- A 2048-page window exposed intermittent embedded `wp_options` corruption in
  repeated WordPress fixture runs, so this slice uses a smaller 1024-page
  window until the larger-window visibility issue is isolated separately.

## Design

- Increase `MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES` from 256 to 1024.
- Keep the existing geometric allocation path, which caps the buffer at the
  same page limit.
- Accept the larger worst-case transient memory per active top-level checkpoint:
  1024 pages at the current 4096-byte page size is 4 MiB.
- Leave rollback, savepoint, read-your-writes, and header-publication behavior
  unchanged.

## Affected Subsystems

- MyLite storage active checkpoint buffering.
- Large transaction row-DML update performance.
- Savepoint rollback over buffered append tails.

## Compatibility Impact

SQL behavior does not change. `ENGINE=InnoDB`, omitted/default, MyISAM, and Aria
routed tables still use the same MyLite storage path and MariaDB still owns
statement and transaction boundaries.

## Single-File And Lifecycle Impact

The buffer is process memory only. Durable state remains in the primary
`.mylite` file plus existing MyLite-owned journal companions. Commit still
flushes append pages before publishing page `0`; rollback still truncates pages
past the restored checkpoint.

## Public API And File-Format Impact

No public API or durable file-format changes.

## Storage-Engine Routing Impact

No routing policy changes.

## Binary-Size Impact

Constant-only first-party C change. No new dependency.

## Tests And Verification

- Add a storage regression that updates enough indexed rows inside one
  transaction to cross the 1024-page flush window, opens a nested savepoint,
  updates additional rows, rolls back the savepoint, commits the retained
  prefix, and verifies row/index visibility plus primary-file size.
- Run the storage-smoke build targets and storage test binary.
- Run focused storage/embedded checkpoint tests and the full storage-smoke
  CTest suite.
- Run a local 100k update performance baseline sample.
- Run `git diff --check` and `git clang-format --diff`.

## Acceptance Criteria

- Large transaction append runs batch across the larger flush window.
- Savepoint rollback after crossing the flush window preserves rows before the
  savepoint and hides rows after it.
- Top-level commit still flushes buffered pages before header publication.
- Existing storage and embedded tests remain green.

## Risks And Open Questions

- The larger buffer trades up to 4 MiB of transient memory for fewer append
  writes. That is acceptable for the current performance-focused profile, but
  a future small-memory profile may need a tunable limit.
- This does not reduce append-only page volume. Full SQLite-like update
  throughput still needs transaction-local replacement coalescing, free-space
  reuse, or a real pager/WAL design.
