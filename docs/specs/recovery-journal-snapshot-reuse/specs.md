# Recovery Journal Snapshot Reuse

## Problem

The accepted prepared-insert smoke profile still reports `1,943` full-page
checksum calls and `1,449` maintained-root decodes. The remaining
maintained-root decode sites are protected paths: `674` planning decodes,
`774` recovery-journal saved-page validation decodes, and `1` durable root read.

Recovery-journal extension currently rereads the existing journal file,
decodes its header, and revalidates already-protected saved pages every time a
new dirty page is added to the same active statement journal. That preserves
safety, but repeats validation work for pages the same statement has already
validated and written to the journal.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage code.
- `begin_journal_at_path()` builds a `mylite_storage_recovery_journal`, reads
  the saved pages, validates all saved pages, and writes the journal file.
- `ensure_existing_journal_protects_pages()` skips pages already listed in
  `statement->journal_dirty_pages`, but when any new page is needed it opens and
  decodes the existing journal, validates all saved pages again, appends the new
  saved page, validates all pages again, and rewrites the journal.
- `validate_recovery_journal_saved_page()` is the protected-page validation
  gate that decodes maintained roots, leaves, branches, rows, catalog pages, and
  other durable page families before the journal rewrite is accepted.

## Design

Carry the owning statement's validated journal snapshot in memory:

- journal page-id list,
- saved header decoded from the journal's header page,
- saved page bytes for the protected pages,
- a `has_snapshot` bit.

When the statement first creates a recovery or transaction journal, store the
validated snapshot only after the journal file is successfully written. When
the same journal is extended, reuse that carried snapshot instead of rereading
and revalidating the old journal file. For each newly protected page, still
read the durable page bytes and run `validate_recovery_journal_saved_page()` on
that new page before writing the rewrite journal and renaming it into place.

If the statement has no carried snapshot, keep the existing file-read and full
validation path. If the rewrite or rename fails, keep the previous snapshot
state and remove the rewrite file, matching the existing on-disk failure shape.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, metadata, or
file-format behavior changes.

## Single-File And Lifecycle Impact

No durable files are introduced. The existing recovery and transaction journal
files keep the same names, contents, rewrite path, rename behavior, and cleanup
behavior. The new snapshot is statement-local process memory only.

## Binary-Size And Dependency Impact

No dependency or license changes. A statement that owns an active journal can
carry a bounded lazy in-memory journal snapshot sized by
`MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`.

## Test And Verification Plan

- Add focused test-hook coverage proving that extending the same active
  recovery journal no longer records `decode_recovery_journal_header`, while
  rollback still restores multiple protected existing pages.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Existing journal extension uses the carried statement snapshot when present.
- Newly added protected pages are still validated before the journal rewrite is
  accepted.
- Fallback file-read validation remains for journals without a carried
  snapshot.
- Prepared-insert benchmark evidence shows reduced recovery-journal header
  decodes and reduced recovery-journal maintained-root validation decodes
  without changing planning decodes.

## Risks

The in-memory snapshot must only be updated after the journal rewrite is safely
published. Updating it earlier would let a later extension believe an on-disk
journal contains pages that were never durably published.

## Implementation Notes

- `mylite_storage_statement` now carries a small journal snapshot descriptor
  and lazily allocates the protected-page byte buffer only after a journal write
  succeeds. This keeps ordinary statement allocation from paying for the
  bounded snapshot storage.
- `begin_journal_at_path()` stores the validated snapshot after the initial
  recovery or transaction journal file has been written.
- `ensure_existing_journal_protects_pages()` reuses the statement snapshot when
  present, validates only newly added protected pages with
  `validate_recovery_journal_saved_page()`, and updates the snapshot only after
  the rewrite journal is written, renamed, and directory-synced.
- Statement cleanup, reusable statement reset, and journal dirty-page cleanup
  clear the snapshot and free the lazy page buffer.

## Evidence

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed with no modified files.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  `330.85 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed; `libmariadbd.a` size was `33,977,714` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `456.40 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported:
  - prepared insert step component: `81.910 us/op`
  - full-page checksum calls: `8`
  - zero-tail checksum calls: `235,291`
  - maintained-root decodes: `677`
  - `read_index_leaf_run_root`: `1`
  - `plan_maintained_index_root_inserts`: `674`
  - `validate_recovery_journal_saved_page`: `2`
  - branch decode sites: `none | 0`
  - dirty refreshes by source: `88,171` dirty-page-flush, `6,849`
    append-buffer-flush, `4` append-buffer-copy
  - dirty publications: `32,266` buffer-limit `index-leaf`, `1`
    statement-commit `index-leaf`, `2` statement-commit `index-branch`,
    `55,902` merge-direct-write `index-leaf`
- Adjacent benchmark samples reported the same structural counters with
  prepared-insert step samples of `81.108 us/op`, `88.268 us/op`, and
  `81.221 us/op`.
