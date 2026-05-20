# Reusable Buffered Page Undo List

## Problem

Every active row-DML statement that rewrites buffered unpublished pages captures
one or more per-statement rollback preimages. The prepared update benchmark
executes many successful statements inside one transaction, and each nested
statement allocates a small buffered-page undo list before freeing it at
statement commit.

After typed undo sizing, the focused prepared-update profile still shows heap
allocation under `capture_buffered_page_undo_with_used_size()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite statement checkpoints live in
  `packages/mylite-storage/src/storage.c::mylite_storage_begin_statement()`,
  `mylite_storage_commit_statement()`, and
  `mylite_storage_rollback_statement()`.
- `capture_buffered_page_undo_with_used_size()` grows
  `statement->buffered_page_undos` from zero capacity on the first captured
  preimage for a statement.
- `free_statement()` calls `clear_buffered_page_undos()` after successful
  commit or rollback, so a small undo array can be recycled only after the
  statement no longer owns rollback state.

## Design

- Keep buffered-page undo entries statement-owned while a statement is active.
- Add one bounded thread-local reusable undo list for the common small undo
  capacity used by successful row-DML statements.
- On first capture, adopt the reusable list when available instead of calling
  `realloc()`.
- On statement cleanup, return a small undo list to the reusable slot when the
  slot is empty; otherwise free it normally.
- Do not cache oversized undo lists, and do not share an undo list between
  active statements.

## Affected Subsystems

- MyLite storage active statement cleanup.
- Active buffered update rewrite rollback bookkeeping.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

No SQL, MySQL/MariaDB API, handler, or storage-engine routing behavior changes.
This changes only transient allocator behavior for rollback bookkeeping.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, flush, or companion-file
lifecycle change. Active statement rollback keeps using statement-owned
preimages until the statement ends.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C cache. No new dependency. The reusable memory is bounded to
the initial small undo-list capacity on a thread.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Run a sampled focused prepared-updates benchmark with macOS `sample` and
  confirm allocator frames under undo capture move down or disappear.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Successful row-DML statement cleanup can reuse a small buffered-page undo list
  for later statements on the same thread.
- Active statements never share mutable undo storage.
- Oversized undo lists are freed instead of retained.
- Existing active update rewrite, savepoint rollback, storage, and embedded
  storage-engine tests remain green.
- Benchmark/profile evidence records the prepared update latency impact.

## Verification Evidence

- With reuse enabled, three focused local runs of
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000` measured prepared primary-key updates at `4.193`, `4.305`, and
  `4.319 us/op`.
- In the same working tree with only this slice temporarily reverted and the
  benchmark rebuilt, three focused local runs measured `4.379`, `4.339`, and
  `4.403 us/op`. Treat this as local evidence, not a portable threshold.
- A three-second macOS `sample` run with reuse enabled no longer showed
  allocator frames under `capture_buffered_page_undo_with_used_size()`; a small
  `adopt_reusable_buffered_page_undos()` frame remained visible.

## Risks And Open Questions

- This intentionally keeps one bounded thread-local allocation alive after a
  statement ends. MyLite already uses thread-local durable read/index/payload
  caches; the retained undo list must stay small and independent from durable
  file identity.
