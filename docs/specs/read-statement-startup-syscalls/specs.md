# Read Statement Startup Syscalls

## Problem

Fresh local sampling still shows hot secondary exact reads spending time in
read-statement startup before the handler reaches cached row/index work. The
largest safe costs are repeated recovery-journal path allocation and cached
read-file identity validation through both `stat()` and `fstat()` on every
read statement.

MyLite cannot skip journal existence checks without weakening cross-process
recovery behavior, but it can remove avoidable allocation and validation work
around those checks.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::initialize_read_statement()` calls
  `recover_pending_journals()` before opening or reusing a read file.
- `recover_pending_journals()` builds recovery and transaction journal path
  strings, then checks both paths before taking an exclusive recovery lock.
- `take_cached_read_file()` reuses a thread-local unlocked read file, but
  `cached_read_file_matches_path()` currently performs both `stat(filename)`
  and `fstat(file)` every time to prove the cached handle still points at the
  same path.
- Existing `test_read_statement_file_cache_path_replacement()` verifies that a
  cached read handle is discarded when the underlying path is replaced.

## Design

- Add a thread-local journal-path cache keyed by database filename. The cache
  stores the deterministic recovery and transaction journal paths and reuses
  them across repeated read-statement startup.
- Keep journal existence checks on every read startup.
- Store device and inode identity when a read file is cached. Later reuse only
  needs to `stat()` the path and compare against the cached identity, avoiding
  `fstat()` on the cached handle for every read statement.
- Keep replacement detection: if the path resolves to a different device/inode,
  clear the cached handle and open the replacement file normally.

## Compatibility Impact

No SQL or API behavior changes. Cross-process recovery checks remain in place,
and path replacement continues to be detected before a cached handle is reused.

## Single-File And Lifecycle Impact

No file-format, journal format, lock, or companion-file change.

## Public API And File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

All durable routed engines using MyLite read statements benefit, including
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default engines, and
`ENGINE=MYLITE`.

## Binary-Size And Dependency Impact

Small first-party C code only. No new dependency.

## Tests And Verification

- Reuse storage coverage for recovery journal cleanup/recovery.
- Reuse cached read-file path replacement coverage.
- Run storage unit tests and routed storage-engine tests.
- Run the local performance baseline to compare point and secondary exact
  reads.
- Run `git diff --check` and formatting checks.

## Acceptance Criteria

- Cached read-file reuse still rejects path replacement.
- Recovery journal checks still run on read-statement startup.
- Existing storage and storage-smoke tests pass.
- The local benchmark does not regress read timings.

## Risks And Open Questions

- The remaining `stat()` journal checks and shared lock acquisition stay in the
  hot path to preserve cross-process recovery and writer exclusion semantics.
- Further startup reductions likely need a broader connection-level read
  transaction model, which would change lock lifetime tradeoffs.
