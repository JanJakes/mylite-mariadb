# Planned Dirty Page Journal Sets

## Goal

Allow a storage mutation to declare the existing pages it may dirty before the
active statement recovery journal is created. Maintained index maintenance will
know candidate root or leaf pages before it appends row/index-entry tail pages;
those page ids must be in the immutable recovery journal before any dirty
write occurs.

The previous dirty-page registration slice lets the first unjournaled dirty
page create a protected recovery journal, but that does not help row mutation
paths that already create a journal for appended pages. This slice adds the
bounded pre-registration contract needed by the next maintained-index page
format.

## Non-Goals

- No maintained B-tree page format, split, merge, or navigation work.
- No production row-DML index page maintenance yet.
- No transaction-journal redesign.
- No WAL, lock-manager, MVCC, or cross-process write-concurrency claim.
- No public `libmylite` or storage API change.
- No durable primary `.mylite` file-format change.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`,
  `update_row()`, and `delete_row()` call first-party storage APIs after
  MariaDB has produced row buffers and key tuples.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  creates a write journal before appending row and index-entry pages.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  creates a write journal before writing replacement row, row-state, and
  changed index-entry pages.
- `packages/mylite-storage/src/storage.c::begin_journal_at_path()` writes the
  full protected-page set once and creates the companion with `O_EXCL`, so the
  set must be known before journal creation.
- `packages/mylite-storage/src/storage.c::begin_recovery_journal_for_pages()`
  already accepts a bounded extra protected-page set.
- `packages/mylite-storage/src/storage.c::capture_dirty_page_undo_for_pager_write()`
  rejects dirty writes after an immutable journal exists unless the page is
  already represented by active dirty-page undo state.

## Compatibility Impact

No SQL-visible behavior should change. The work is an internal storage safety
contract for future maintained pages. Unsupported internal dirty-write shapes
continue to fail instead of silently weakening crash recovery.

`docs/COMPATIBILITY.md` does not need a new user-visible claim because
maintained indexes remain planned.

## Design

Track the dirty page ids protected by an active statement journal:

- add a small statement-owned protected dirty-page-id list;
- when a caller creates a recovery journal with an explicit protected-page set,
  copy those page ids into the statement list after journal creation succeeds;
- when the first dirty page creates a journal opportunistically, record that
  page id in the same list;
- make the pager dirty-write guard check the protected-page-id list, not only
  the in-memory dirty undo list;
- allow repeated writes to a protected page in nested savepoints while each
  savepoint still captures its own in-memory rollback preimage;
- reject pages not in the protected list once a statement or transaction
  journal already exists; and
- release the list with the active checkpoint state.

The first implementation should expose this through private storage helpers and
test-only hooks. Production maintained-index work can then call the helper from
row mutation planning once the target leaf/root page ids are known.

## Initial Implementation

The first implementation adds a statement-owned list of dirty page ids that
were explicitly included in the active recovery journal. Existing
`begin_write_journal_for_statement()` calls delegate to a new private helper
that can accept an optional protected-page set. After journal creation succeeds,
that helper records the explicit page ids on the owning statement. The
opportunistic first dirty-page path records its page id in the same list.

The pager dirty-write guard now allows a dirty write when the page is present
in the active statement chain's protected-page list, then still captures the
per-statement in-memory preimage for rollback. If a journal already exists and
the page is not in the protected list, the write remains
`MYLITE_STORAGE_UNSUPPORTED`.

Storage unit coverage uses a build-testing-only hook to pre-register a pair of
row pages, dirty both through the pager hook, and roll back the statement.

## File Lifecycle

No new files. The existing transient `-journal` companion remains immutable
after creation, but its protected-page list can now be created from a
preplanned page set. Cleanup remains statement commit, rollback, recovery on
open, or failed creation cleanup.

## Embedded Lifecycle And API

No public API change. The helper stays private to `packages/mylite-storage`.
MariaDB handler savepoint and transaction hooks keep their current shape.

## Build, Size, And Dependencies

No new dependency and no MariaDB-derived source change. Binary-size impact is
limited to a small dynamic page-id list and build-testing-only hooks.

## Test Plan

- Add a storage unit test that pre-registers two existing row pages in an
  active statement, dirties both through the pager test hook, and rolls back to
  prove both page images restore.
- Keep the rejection test for dirtying an unprotected existing page after
  journal creation.
- Keep the child-process recovery test for the opportunistic single-page path.
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

- An active statement can create a recovery journal with a bounded preplanned
  dirty-page set.
- Dirty writes to pages in that set are allowed and still capture statement or
  savepoint in-memory preimages.
- Dirty writes to pages outside that set are rejected once the journal is
  immutable.
- Existing dirty-page rollback, crash recovery, and storage-smoke tests pass.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Risks And Open Questions

- The protected set remains bounded by
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`.
- Active durable transactions still need a transaction-aware dirty-page
  protection contract; this slice only prepares ordinary active statement
  journals.
- Maintained indexes still need page-selection logic before row DML can call
  the planned-page helper in production.
