# Empty Protected Page Journal Fast Path

## Problem

Prepared primary-key update profiling still shows
`begin_write_journal_for_statement_pages()` after the statement journal-owner
lookup was inlined. In the hot transaction path for ordinary row/index updates,
the current statement chain already owns a transaction journal and the mutation
does not need additional maintained-root protected pages.

`begin_write_journal_for_statement_pages()` still constructs a pager and calls
`ensure_existing_journal_protects_pages()`, which immediately returns when
`protected_page_count == 0`. That is a redundant call boundary and pager setup
on every such prepared update.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage in
  `packages/mylite-storage/src/storage.c`.
- `update_row_with_index_entries()` calls
  `begin_write_journal_for_statement_pages()` before active buffered update
  rewrites.
- `begin_write_journal_for_statement_pages()` reuses an existing transaction or
  recovery journal when one is present in the active statement chain.
- `ensure_existing_journal_protects_pages()` returns `MYLITE_STORAGE_OK`
  before validating pager and journal path arguments when `page_count == 0`.
- Statements that do not already own a write journal still need the existing
  recovery-journal creation path even when no extra protected pages are passed.

## Design

- After resolving transaction and recovery journal owners, return immediately
  when a journal owner exists and `protected_page_count == 0`.
- Leave statement-owned journal creation unchanged when no owner exists.
- Leave protected-page validation, journal rewrites, journal dirty-page
  tracking, and recovery/transaction journal filenames unchanged when protected
  pages are present.

## Affected Subsystems

- Statement and transaction write-journal protection.
- Prepared primary-key update storage hot path.

## Compatibility Impact

No SQL, handler API, public C API, storage-engine routing, metadata, or
file-format behavior changes. This removes only work that the downstream
protected-page helper already skipped.

## Single-File And Lifecycle Impact

No durable file or companion-file lifecycle change. Existing statements still
create, reuse, rewrite, commit, roll back, and remove transaction/recovery
journals through the same paths.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Tiny first-party branch. No dependency or build-profile change.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Rebuild `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` and
  relink the embedded smoke binaries.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check the remaining
  write-journal frames.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

Completed verification:

- Built `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset, rebuilt
  `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`, and relinked
  the embedded smoke binaries against the rebuilt archive.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed: 2 tests, 34.95 seconds.
- `ctest --preset storage-smoke-dev --output-on-failure` passed: 10 tests,
  39.99 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` measured prepared primary-key updates
  at 2.442 us/op.
- A sampled prepared-update run showed `begin_write_journal_for_statement_pages()`
  as a small remaining frame, but did not show `open_storage_pager()` under the
  empty protected-page path.
- `git diff --check` passed, and `git clang-format --diff --
  packages/mylite-storage/src/storage.c` did not modify any files.

## Acceptance Criteria

- Existing transaction/recovery journal owners skip empty protected-page work.
- Statements without an existing journal owner still create the statement
  recovery journal through the existing path.
- Protected-page journal rewrites still run when maintained-root pages are
  supplied.
- Existing storage and embedded storage-engine tests pass.

## Risks And Open Questions

- This is intentionally narrow. It does not reduce the dominant buffered row
  rewrite and rollback-preimage work still present in prepared-update samples.
