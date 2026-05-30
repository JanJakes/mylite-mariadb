# Ownerless Page-Version Before Append Fault

## Problem Statement

Phase 9 crash coverage tracks page-version append boundaries. Existing unsafe
coverage kills a writer after the page-version WAL append but before the shared
page-version index publish. The earlier boundary, before any page-version WAL
record is appended for the page being published, still needs deterministic
fault coverage.

This slice adds a test-only pause before the page-version append and SQL
coverage proving that no-live-process recovery remains readable and a later
ownerless writer can proceed after the interrupted publish.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `packages/libmylite/src/database.cc`
  - `ownerless_innodb_page_publish_hook()` is the first-party page-version
    publish callback installed into the MyLite InnoDB hook surface.
  - It validates the hook context, appends a page image to
    `mylite-concurrency.wal` with `mylite_ownerless_page_log_append_at()`,
    then publishes the WAL offset into the shared page-version index.
  - The existing unsafe `page-publish-after-append` pause sits after the WAL
    append and before index publication.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `test_crashed_page_publish_rebuilds_ownerless_state()` already covers the
    after-append boundary by killing an autocommit writer and proving recovery
    remains readable before and after forced `.shm` rebuild.

## Design

Add an unsafe-test-only pause named `page-publish-before-append` immediately
before `ownerless_innodb_page_publish_hook()` appends the page-version WAL
record. Add a sibling SQL crash test to the existing after-append coverage:

1. Create the standard ownerless test database.
2. Start an ownerless writer that updates row `id=1` and pauses before the
   page-version append for the publish path.
3. Kill the writer at that pause.
4. Reopen with no live peers and accept either the baseline total or the
   committed update total, because this boundary is inside the native
   commit/page-publish path.
5. Run a later ownerless writer and prove the directory remains writable and
   readable.
6. Force `.shm` rebuild and prove the recovered total remains one of the
   accepted durable states.

## Scope

In scope:

- A first-party unsafe test pause before page-version WAL append.
- Ownerless SQL crash coverage for the pre-append page-version boundary.
- Spec, compatibility, and Phase 9 checklist updates.

Out of scope:

- Changing page-version WAL format, page-index format, or replay selection.
- Claiming deterministic commit outcome at this crash boundary.
- Native redo/checkpoint reclamation policy changes.

## Compatibility Impact

No public SQL or C API behavior changes. The pause is compiled only when unsafe
ownerless test hooks are enabled and only activates when the matching test
environment variable is set.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing no-live-process
recovery and forced `.shm` rebuild over the existing `mylite-concurrency.wal`
and `mylite-concurrency.ckpt` anchors.

## Native Storage Impact

The interrupted writer may have reached a native commit/publish boundary where
the durable outcome is either the previous committed page image or the updated
page image. The test therefore does not claim one outcome; it proves the
directory remains recoverable and later writes are not blocked by the
interrupted page-version publish.

## Binary Size Impact

Adds one unsafe-test-only pause call in first-party code and one SQL test case.
No dependency, public symbol, or durable state is added.

## Test Plan

- Build the `ownerless-test-hooks` SQL target.
- Run the new `page-publish-before-append-crash` selector.
- Run the ownerless hook CTest filter covering cross-process SQL and negative
  proof tests.
- Rebuild and run the normal embedded ownerless SQL filter.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The writer is killed before the page-version append for the publish path.
- No-live recovery reopens the directory and observes either accepted durable
  total.
- A later ownerless writer can update and close successfully.
- Forced `.shm` rebuild remains readable.
- Existing ownerless hook and embedded SQL coverage remains green.

## Risks And Open Questions

- The accepted outcome is intentionally nondeterministic at this boundary; this
  is recovery-hardening coverage, not a commit-decision test.
- This does not replace the after-append/index-publish fault test.
