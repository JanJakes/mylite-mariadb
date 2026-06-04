# Ownerless Dictionary Rename Crash

## Problem

Ownerless dictionary crash coverage proved `CREATE TABLE` boundaries, but the
remaining DDL/file-lifecycle gap specifically calls out live-peer recovery for
operations that move native files. `RENAME TABLE` is a narrower file-lifecycle
case than broader randomized DDL recovery and already routes through the
ownerless dictionary begin/finish hooks.

## Design

Add hook-only SQL coverage for a writer killed after MariaDB completes a
same-schema InnoDB `RENAME TABLE` but before MyLite finishes the ownerless
dictionary generation:

1. Create and populate `app.ownerless_rename_crash_source`.
2. Hold a live ownerless peer open.
3. Run `RENAME TABLE app.ownerless_rename_crash_source TO
   app.ownerless_rename_crash_target` with the existing
   `dictionary-before-finish` unsafe hook armed.
4. Kill the writer at that hook boundary.
5. Prove a new ownerless opener remains busy while the live peer is present.
6. Release the peer, reopen ownerless, and verify the target table exists,
   the source table is absent, and the moved row remains readable.
7. Force `.shm` rebuild and verify the moved table remains usable through both
   ownerless and ordinary native read/write reopen.

No production code change is needed for this slice; the existing no-live
dictionary recovery and native file-operation checkpoint paths already preserve
the same-schema rename final state.

## Compatibility Impact

No SQL syntax or public C API change. The evidence extends ownerless DDL crash
coverage from create-only dictionary boundaries to a native file move.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the
  `ownerless-test-hooks` preset.
- Run the focused `dictionary-rename-crash` selector.
- Run the adjacent hook crash-tail and native reclaim selectors.
- Run focused embedded rename/native reopen selectors to keep normal
  non-hook DDL behavior covered.
