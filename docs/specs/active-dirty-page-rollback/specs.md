# Active Dirty Page Rollback

## Goal

Add the first pager-owned rollback boundary for in-place writes to existing
typed pages during active MyLite storage statements. Maintained B-tree roots,
row directories, and future free-space reuse need this before row DML can
rewrite durable pages instead of appending replacement state.

The first implementation should be deliberately narrow: capture and restore
bounded full-page preimages for existing pages written through the internal
pager while a statement or savepoint is active.

## Non-Goals

- No B-tree page format, split, merge, or navigation work.
- No WAL, lock-manager, MVCC, or cross-process write-concurrency claim.
- No public `libmylite` or storage API change.
- No durable `.mylite` file-format change.
- No replacement for append-buffer undo. Unpublished append-buffer rewrites
  keep their existing lighter undo path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc::trans_commit_stmt()` and
  `trans_rollback_stmt()` treat the statement transaction as a savepoint-like
  boundary and route work through handler transaction hooks.
- `mariadb/sql/transaction.cc::trans_savepoint()`,
  `trans_rollback_to_savepoint()`, and `trans_release_savepoint()` call
  `ha_savepoint()`, `ha_rollback_to_savepoint()`, and
  `ha_release_savepoint()`.
- `mariadb/sql/handler.h::handlerton::savepoint_offset`,
  `savepoint_set`, `savepoint_rollback`, and `savepoint_release` define the
  storage-engine savepoint memory contract.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_savepoint_set()`,
  `mylite_savepoint_rollback()`, and `mylite_savepoint_release()` already map
  MariaDB savepoints to nested MyLite storage checkpoints.
- `packages/mylite-storage/src/storage.c::begin_write_journal_for_statement()`
  creates one recovery journal for an active statement chain and materializes
  statement-start catalog state when catalog writes are involved.
- `packages/mylite-storage/src/storage.c::mylite_storage_rollback_statement()`
  restores statement-start header/catalog pages, buffered append-page undo,
  auto-increment rollback values, and file length.
- `packages/mylite-storage/src/storage.c::pager_write_page()` is now the
  common write path for row pages, row-state pages, autoincrement pages, BLOB
  payload pages, free-list pages, append-only index-entry pages, and index leaf
  pages.
- `packages/mylite-storage/src/storage.c::allocate_catalog_page_run()` already
  rewrites an existing free-list page when catalog publication reuses part of a
  reclaimed catalog-chain run. That gives this slice a concrete, typed
  in-place write to test before B-tree pages exist.

Official MariaDB documentation for SAVEPOINT and handler transactions supports
the same engine-level rollback model; the local source refs above are the
implementation authority for this base line.

## Compatibility Impact

No SQL-visible behavior should change except fixing rollback correctness for
storage-internal in-place typed-page rewrites. Existing MySQL/MariaDB
statement, transaction, and savepoint semantics remain the contract.

`docs/COMPATIBILITY.md` does not need a new user-visible support claim for this
slice because maintained B-tree pages and broader write concurrency remain
planned.

## Design

Extend active storage checkpoints with a second undo list for published dirty
pages:

- capture a full-page preimage once per active statement/savepoint and page id;
- capture only existing addressable pages, never new pages at or beyond the
  statement-start header page count;
- skip pages that are still in the active append buffer because they already
  use buffered-page undo and truncate semantics;
- restore dirty-page preimages during statement rollback before truncating the
  primary file;
- keep rollback restore order deterministic by preserving capture order;
- clear parent statement caches when a nested rollback restores published
  pages; and
- keep page-family validation in existing typed decoders, not in the undo list.

Crash recovery for durable in-place rewrites remains tied to the existing
recovery journal. The first implementation may rely on the active statement's
current journal for catalog/free-list rewrite coverage because that journal now
protects bounded typed pages. Later maintained index work should pass dirty
page ids into the protected-page journal before flushing pages that are not
already included by catalog/free-list publication.

## File Lifecycle

The primary `.mylite` file remains the only durable database asset. The
existing recovery and transaction journal companions keep their current names
and cleanup behavior. This slice stores statement dirty-page preimages only in
process memory and removes them at statement commit, rollback, release, or
close.

## Embedded Lifecycle And API

No public API change. MariaDB handler savepoint hooks keep creating nested
first-party storage checkpoints. Repeated `mylite_open()` / `mylite_close()`
behavior is unaffected.

## Build, Size, And Dependencies

No new dependency and no MariaDB-derived source change is required. Binary-size
impact should be limited to small private storage helpers and one undo list on
active storage checkpoint state.

## Test Plan

- Add a storage unit test that creates a reclaimable catalog free-list run,
  opens an active statement, forces partial free-list reuse through catalog
  publication, rolls the statement back, and asserts the free-list root page
  bytes are restored.
- Keep the existing active append-buffer savepoint rollback tests passing.
- Keep protected-page journal recovery tests passing.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

- If the storage-smoke build remains configured, also run:

```sh
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

## Acceptance Criteria

- Active statement rollback restores existing typed pages dirtied through the
  pager.
- Nested savepoint rollback restores pages dirtied after the savepoint without
  discarding earlier parent dirty state.
- New appended pages remain governed by the existing header/truncate and
  append-buffer rollback paths.
- No public API or durable file-format change is introduced.
- Storage and storage-smoke verification pass.

## Risks And Open Questions

- The first dirty-page undo list is in-memory and bounded only by allocation
  limits. Maintained B-tree work may need an explicit page-count cap or spill
  strategy before high-volume in-place rewrites are enabled.
- Crash recovery for arbitrary dirty pages is not fully wired until production
  callers pass protected page ids into the journal before flush. This slice
  should not claim B-tree-safe recovery until that connection exists.
- Future WAL/checkpoint work may replace this rollback-journal model for
  high-concurrency writers.
