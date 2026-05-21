# Maintained Index Root Transaction Rollback

## Problem

Maintained index roots are now mutable storage pages for eligible insert,
update, and delete paths. The general pager and rollback-journal machinery can
protect dirty existing pages, but the maintained-root paths need direct
coverage that proves statement rollback, nested savepoint rollback, transaction
rollback, and crash recovery restore root bytes and index visibility.

Without that evidence, expanding maintained indexes into larger navigable
structures would compound an unproven durability surface.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_begin_statement_checkpoint()`,
  `mylite_finish_statement_checkpoint()`, `mylite_savepoint_set()`,
  `mylite_savepoint_rollback()`, `mylite_commit()`, and
  `mylite_rollback()` bridge MariaDB statement, savepoint, and transaction
  lifecycles to MyLite storage statements.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`,
  `update_row()`, and `delete_row()` serialize supported key images and call
  the MyLite storage insert, update, and delete entry points.
- `packages/mylite-storage/src/storage.c::begin_write_journal_for_statement_pages()`
  can create a journal protecting a bounded page set before dirty pages are
  written.
- `packages/mylite-storage/src/storage.c::pager_write_page()` registers
  preimages for later dirty existing-page writes when the active statement owns
  a compatible recovery or transaction journal.
- Maintained root insert, update, and delete paths predeclare root page ids and
  rewrite those roots through the pager.

## Scope

- Add storage-level coverage for maintained root insert, update, and delete
  mutations inside active statements and transactions.
- Verify rollback restores exact root bytes, root metadata counts, exact index
  reads, full index reads, and prefix checks.
- Verify nested savepoint rollback restores maintained root bytes while the
  outer transaction can still commit earlier maintained-root changes.
- Verify crash recovery restores dirty maintained root pages when a child
  process exits after mutating a root but before checkpoint commit.
- Fix any discovered gaps in root-page dirty tracking, rollback propagation,
  or read visibility.

## Non-Goals

- No new index page format.
- No root split, merge, B-tree navigation, or free-list reuse.
- No SQL syntax or public API change.
- No broad MariaDB transaction-behavior expansion beyond maintained-root page
  durability.

## Design

The tests should create a small fixed-width indexed table, publish maintained
single-page roots, snapshot the root pages, then exercise each mutation class:

- insert: append a new row whose index entries are eligible for root insertion;
- update: replace a row with changed primary and secondary key bytes;
- delete: delete a row whose entries live in maintained roots.

For statement rollback and transaction rollback, the assertions should compare
root page bytes before and after rollback and then verify logical visibility
through exact-entry reads, indexed-row reads, full index reads, and prefix
existence checks. For savepoint rollback, an outer transaction should commit a
prior root mutation, roll back a later nested mutation, and then commit the
outer transaction. For crash recovery, a child process should begin a storage
statement, perform a maintained-root mutation, leave the journal in place by
exiting without commit or rollback, and the parent should verify recovery on the
next read.

The implementation should prefer tests over new production code. If a failing
test exposes a production bug, fix the narrow root cause in storage journal or
root mutation code.

## Compatibility Impact

SQL-visible behavior should not change. The slice strengthens confidence that
MyLite's existing MariaDB statement, savepoint, and transaction mapping remains
correct now that index roots can be dirty existing pages rather than append-only
history.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The existing
MyLite-owned recovery and transaction journal companions remain the only files
involved, and tests should assert those companions are removed after rollback,
commit, or recovery.

## Public API And File-Format Impact

No public API or file-format change is expected. If implementation work needs a
test-only helper, keep it behind the existing storage test hooks.

## Storage-Routing Impact

No engine-routing policy changes. The embedded smoke may add SQL-level evidence
against routed MyLite tables if storage-level tests expose a gap worth
covering at the handler boundary.

## Binary-Size, License, And Dependency Impact

No new dependencies or imported code. Binary size should not change except for
test binaries and any narrow production fix required by failed tests.

## Test Plan

- Storage unit tests:
  - statement rollback restores maintained root insert, update, and delete
    page bytes and visibility;
  - transaction rollback restores maintained root page bytes and visibility;
  - nested savepoint rollback restores later maintained root changes while
    preserving earlier committed-to-outer-transaction root changes;
  - recovery from an abandoned statement journal restores dirty maintained root
    pages.
- Embedded smoke only if a storage bug fix changes handler-observable behavior.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Maintained root insert, update, and delete mutations are covered by rollback
  and recovery tests.
- Tests prove both physical root bytes and logical index visibility are
  restored.
- No append-only fallback is used merely to avoid dirty-root rollback.
- Docs and roadmap no longer describe transaction-aware maintained index root
  mutation as unproven for the single-page maintained-root scope.

## Initial Implementation

- `packages/mylite-storage/src/storage.c` now keeps catalog-backed index roots
  available for exact lookups even when active table-entry metadata is served
  from cache.
- Active exact-index cache seeding and published-root cache reads bypass stale
  durable cache state while the active statement chain has deferred durable
  cache retargeting for the table being mutated.
- Maintained root reads treat the single-page root as the authoritative entry
  set, avoiding append-tail row-state overlays for entries already updated in
  place.
- Dirty maintained-root writes inside active durable transactions extend the
  transaction journal through a replacement journal and atomic rename before
  the primary root page is rewritten. In-process dirty-page undo still provides
  statement, savepoint, and transaction rollback.
- Storage tests cover statement rollback for insert, update, and delete;
  transaction rollback; nested savepoint rollback; abandoned statement
  recovery; and abandoned transaction recovery. Each recovery path compares
  root page bytes and logical exact/full/prefix index visibility.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`

## Risks And Open Questions

- Cross-process transaction snapshots currently use transaction-journal
  snapshots over header and catalog pages. Maintained root dirty pages are
  journal-protected for stale-transaction recovery, but reader visibility for
  those protected pages beyond recovery should become a separate isolation
  slice rather than being hidden here.
- Future multi-page B-tree roots will need broader dirty-page and split/merge
  coverage; this slice only proves the current single-page maintained-root
  mutation model.
- Large maintained-index mutations should pre-register bounded dirty-page sets
  instead of relying on repeated transaction-journal replacement.
