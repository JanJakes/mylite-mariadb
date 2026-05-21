# Pager Protected Page Journal

## Problem

The first pager foundation centralizes typed storage page reads and writes, but
rollback journals still protect only the header page, catalog root page, and
current free-list root page. Maintained B-tree and row-directory pages need a
pager-owned way to protect an arbitrary bounded set of existing pages before
dirty in-place writes begin.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/transaction.cc::trans_commit_stmt()` and
  `trans_rollback_stmt()` route statement boundaries through
  `ha_commit_trans()` and rollback hooks.
- `mariadb/sql/handler.cc::ha_savepoint()` and
  `ha_rollback_to_savepoint()` call handlerton savepoint hooks for engines
  participating in a transaction.
- `packages/mylite-storage/src/storage.c::begin_journal_at_path()` currently
  writes a fixed rollback journal header and up to three saved pages.
- `packages/mylite-storage/src/storage.c::validate_recovery_journal_saved_page()`
  currently accepts only the catalog root and free-list root after the header.

## Design

Extend the transient rollback journal format to version `2` with room for a
bounded protected-page set. Version `1` journals remain readable so an old
pending journal can still recover after a binary update.

The storage layer should:

- encode new rollback journals with a larger protected-page-id area;
- keep the header page as protected page `0`;
- de-duplicate requested page ids before writing the journal;
- reject protected page ids outside the current file page count;
- restore every protected page before truncating the file to the saved header;
- validate saved pages by their existing typed page decoders, not by trusting
  page ids alone; and
- keep page protection internal to storage and pager work, with no public API
  or SQL-visible behavior change.

## Non-Goals

- No maintained B-tree page format in this slice.
- No WAL, checkpoint, or lock-manager redesign.
- No change to the durable `.mylite` file format. The rollback journal is a
  transient companion.
- No claim of full write concurrency.

## Compatibility Impact

MariaDB-visible SQL and handler behavior should be unchanged. The work only
extends crash recovery coverage for MyLite-owned transient journals.

## Single-File And Lifecycle Impact

The primary `.mylite` file remains the only durable database artifact. The
existing rollback journal companion keeps the same lifecycle: create before
primary-file mutation, flush, restore on open after an unclean exit, remove
after recovery or successful publication.

## Public API And File-Format Impact

No public API change. No durable primary-file format change. The transient
rollback journal header version is bumped to `2`; version `1` pending journals
remain accepted for recovery.

## Test And Verification Plan

- Add a storage unit test that writes a recovery journal protecting more than
  the old three-page limit and restores multiple row pages.
- Keep existing catalog/free-list journal recovery tests passing.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- New rollback journals can protect more than three pages.
- Existing version `1` rollback journals remain decodable.
- Recovery validates and restores protected typed storage pages.
- Existing storage and storage-smoke tests pass.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Risks And Unresolved Questions

- The protected-page set is still bounded and copied into the rollback journal.
  A later WAL/checkpoint design may replace this for high-volume dirty-page
  workloads.
- Future B-tree page types must add typed validation before pager dirty writes
  can protect them.
