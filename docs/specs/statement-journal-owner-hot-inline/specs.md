# Statement Journal Owner Hot Inline

## Problem

Prepared primary-key update profiling still shows
`begin_write_journal_for_statement_pages()` and its journal-owner chain lookup
as a small storage-side frame. In the repeated prepared-update path,
`begin_write_journal_for_statement_pages()` checks the active statement chain to
reuse an existing transaction or recovery journal before protecting rewritten
buffered pages.

The owner scan is intentionally small, but it runs on every prepared update and
should inline into the write-journal gate.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `update_row_with_index_entries()` calls
  `begin_write_journal_for_statement_pages()` before active update page
  rewrites.
- `statement_chain_journal_owners()` scans the current statement and parents,
  returning the first recovery-journal owner and first transaction-journal
  owner.
- The function does not allocate, mutate, or perform I/O; it only reads the
  statement ownership flags.

## Design

- Mark `statement_chain_journal_owners()` as `MYLITE_STORAGE_HOT_INLINE`.
- Leave journal ownership, protected-page handling, and journal I/O unchanged.

## Affected Subsystems

- Statement and transaction write-journal protection.
- Prepared primary-key update storage hot path.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, journal
semantics, or file-format behavior change.

## Single-File And Lifecycle Impact

No durable storage or companion-file lifecycle change. This keeps the same
recovery and transaction journal files and only changes the call shape of the
owner lookup.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Tiny first-party inline annotation. No dependency or build-profile change.

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
- Sample a focused prepared-update benchmark and check whether
  `statement_chain_journal_owners()` remains visible.
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
  passed: 2 tests, 33.56 seconds.
- `ctest --preset storage-smoke-dev --output-on-failure` passed: 10 tests,
  42.03 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` measured prepared primary-key updates
  at 2.375 us/op.
- A sampled prepared-update run measured 2.411 us/op. The sample did not show
  `statement_chain_journal_owners()` as a visible frame. Remaining storage
  samples were concentrated in `update_row_with_index_entries()`,
  `rewrite_active_update_pages()`, and
  `begin_write_journal_for_statement_pages()`, with handler-side key-copy work
  still visible.

## Acceptance Criteria

- Repeated write-journal owner scans inline into the write-journal gate.
- Transaction and recovery journal protection behavior remains unchanged.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This removes only a small owner-lookup frame. The dominant remaining storage
  cost is still active update page rewrite and rollback preimage capture.
