# Dirty Page Journal Registration

## Goal

Connect pager-owned dirty writes to the recovery journal before maintained
index pages start rewriting existing durable pages. Active dirty-page rollback
now restores in-process statement and savepoint boundaries, but crash recovery
still needs the rollback journal to contain the original bytes for any existing
page written through the pager.

The first implementation was intentionally narrow: allow one newly dirtied
existing page in an ordinary active statement to create the recovery journal
with that page protected, and reject unsafe dirty writes once an existing
statement journal can no longer be extended. Active durable transactions now
extend their transaction journal through a rewritten replacement journal before
pager-backed existing pages are dirtied.

## Non-Goals

- No mutable B-tree page format, split, merge, or navigation work.
- No broad dirty-page set planner for maintained indexes.
- No transaction-journal redesign.
- No WAL, lock-manager, MVCC, or cross-process write-concurrency claim.
- No public `libmylite` or storage API change.
- No durable primary `.mylite` file-format change.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc::trans_commit_stmt()` and
  `trans_rollback_stmt()` route statement boundaries through
  `ha_commit_trans()` and rollback hooks.
- `mariadb/sql/handler.cc::ha_savepoint()` and
  `ha_rollback_to_savepoint()` call storage-engine savepoint hooks.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_savepoint_set()`,
  `mylite_savepoint_rollback()`, and `mylite_savepoint_release()` map MariaDB
  savepoints to nested MyLite storage checkpoints.
- `packages/mylite-storage/src/storage.c::begin_journal_at_path()` writes the
  full protected-page list into a new transient journal and opens it with
  `O_EXCL`; after creation, callers cannot append another protected page.
- `packages/mylite-storage/src/storage.c::begin_recovery_journal_for_pages()`
  already accepts an arbitrary bounded protected-page list.
- `packages/mylite-storage/src/storage.c::begin_write_journal_for_statement()`
  currently creates one active statement recovery journal with header,
  catalog, and current free-list root pages, but no arbitrary dirty page ids.
- `packages/mylite-storage/src/storage.c::pager_write_page()` is the common
  write path that captures active dirty-page in-memory preimages before
  writing an existing page.
- `packages/mylite-storage/src/storage.c::mylite_storage_commit_statement()`
  and `mylite_storage_rollback_statement()` already own active checkpoint
  journal cleanup and dirty-page undo merge/restore.

## Compatibility Impact

No SQL-visible behavior should change. The slice tightens internal crash
recovery safety for future storage pages and should fail unsafe internal dirty
writes with a storage error instead of silently writing bytes that recovery
cannot restore.

`docs/COMPATIBILITY.md` does not get a new user-visible support claim because
maintained B-tree pages and full transactional dirty-page maintenance remain
planned.

## Design

Add a pager dirty-write guard before the first write to an existing page:

- keep the existing active dirty-page in-memory undo list as the statement and
  savepoint rollback mechanism;
- before capturing the first in-memory preimage for a page, ensure crash
  recovery has a protected-page journal entry for that page;
- if no active statement or transaction journal exists, create the recovery
  journal with header, catalog, current free-list root, and the dirty page id;
- if an active transaction journal exists, rewrite it through a temporary
  replacement journal and atomic rename so the transaction-start page preimage
  is protected before the primary file write;
- keep the recovery journal immutable after creation;
- allow repeated writes to a page already captured in the active statement
  chain;
- reject a different newly dirtied existing page when an immutable statement
  journal already exists and did not protect it; and
- skip new append-buffer pages and pages at or beyond the statement-start page
  count because header rollback and truncation already remove them.

This is not the final maintained-index registration model. Navigable indexes
need an explicit page-set planning step before a mutation starts when more than
one existing page may be dirtied. The bounded guard is still useful because it
turns accidental unsafe page rewrites into test-visible failures and provides a
crash-recovery proof for the first single-page maintained-root experiments.

## Initial Implementation

The first implementation wires the guard into
`packages/mylite-storage/src/storage.c::pager_write_page()` through the
existing active dirty-page preimage path. When an ordinary top-level active
statement dirties one existing page before any statement or transaction journal
exists, storage creates the recovery journal with that page id included in the
bounded protected-page set, then captures the in-memory rollback preimage and
writes the page. Repeated writes to the same page remain allowed because the
active statement chain already has both the recovery-journal page protection
and the in-memory undo preimage.

If a statement journal already exists and the page has not already been
captured in the active statement chain, the write returns
`MYLITE_STORAGE_UNSUPPORTED`. Active durable transactions now keep crash
recovery by replacing the transaction journal with an extended journal that
contains the transaction-start preimage for newly dirtied existing pages before
those writes reach the primary file.

## File Lifecycle

The primary `.mylite` file remains the only durable database asset. The
existing transient `-journal` companion may now contain one additional
protected dirty page when an active statement dirties an existing page before
any other recovery journal is created. Cleanup stays at statement commit,
rollback, recovery on open, or failed journal creation cleanup.

## Embedded Lifecycle And API

No public API change. The first implementation remains entirely inside
`packages/mylite-storage`. MariaDB handler savepoint and transaction hooks keep
their current shape.

## Build, Size, And Dependencies

No new dependency and no MariaDB-derived source change is required. Binary-size
impact is limited to private storage helpers and tests.

## Test Plan

- Extend the active dirty-page rollback storage test to assert the recovery
  journal exists after the first dirty existing-page write and is removed after
  rollback.
- Add a child-process storage test that begins an active statement, dirties an
  existing row page through the pager test hook, exits without rollback, and
  proves reopening recovers the original row page from the protected journal.
- Add a storage test that dirties an existing row page after the active
  statement has already created an immutable journal without that page and
  asserts the dirty write is rejected.
- Keep existing protected-page journal and active checkpoint tests passing.
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

- The first active dirty write to an existing page creates a recovery journal
  that protects that page before the write reaches the primary file.
- Crash recovery restores that dirty page after a process exits without
  statement commit or rollback.
- A dirty write that cannot be journal-protected is rejected rather than
  silently becoming crash-unsafe.
- Existing active rollback and protected-page journal tests pass.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Risks And Open Questions

- The recovery journal is still bounded by
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`; broad maintained-index
  mutations need a planned dirty-page set or a later WAL/checkpoint design.
- Active durable transactions now have a transaction-aware protected-page
  contract for current pager-backed dirty existing pages. Broader multi-page
  navigable index work still needs bounded dirty-page planning so a large
  mutation does not repeatedly rewrite the transaction journal.
- Nested savepoints that introduce the first dirty page under a parent
  checkpoint need careful journal ownership before dirty pages can be broadly
  supported. The first implementation should stay conservative and reject
  unsafe shapes rather than guessing.
