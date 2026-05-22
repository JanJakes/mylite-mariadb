# Table Index Root Absence Hot Inline

## Problem

Prepared primary-key update profiling still shows
`table_index_roots_absent_in_statement()` as a small storage-side frame. The
hot update path has already cached that the active table has no maintained
index roots, so the repeated check is a narrow statement-cache hit.

The helper is behaviorally simple and should disappear into the row-update
call site rather than remain as a sampled call frame.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `update_row_with_index_entries()` checks
  `table_index_roots_absent_in_statement()` before deciding whether catalog
  materialization and maintained-root planning are needed.
- `store_table_index_roots_absent_in_statement()` records the absence after a
  catalog read proves that no index root exists for the table.
- The repeated prepared-update benchmark path reads only the statement-local
  absence descriptor: catalog root page, catalog generation, and table id.

## Design

- Mark `table_index_roots_absent_in_statement()` as
  `MYLITE_STORAGE_HOT_INLINE`.
- Leave the stored absence descriptor and all catalog-planning behavior
  unchanged.

## Affected Subsystems

- Row-update maintained-index-root planning gate.
- Prepared primary-key update storage hot path.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. This is
only an inline/call-shape change for a transient statement cache.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Tiny first-party inline annotation. No dependency or build-profile change.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether
  `table_index_roots_absent_in_statement()` remains visible.
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
  passed: 2 tests, 36.27 seconds.
- `ctest --preset storage-smoke-dev --output-on-failure` passed: 10 tests,
  41.79 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` measured prepared primary-key updates
  at 2.323 us/op.
- A sampled prepared-update run measured 2.320 us/op. The sample did not show
  `table_index_roots_absent_in_statement()` as a visible frame. Remaining
  storage samples were concentrated in `update_row_with_index_entries()`,
  `rewrite_active_update_pages()`, and
  `begin_write_journal_for_statement_pages()`, with handler-side key-copy work
  still visible.

## Acceptance Criteria

- Repeated maintained-root absence checks return through the inlined
  statement-cache hit.
- Catalog materialization and maintained-root planning decisions remain
  unchanged.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This removes only a small call frame. The dominant remaining storage cost is
  still active update page rewrite and rollback preimage capture.
