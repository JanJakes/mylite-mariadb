# Lean Read Statement Reset

## Problem

After hot read-checkpoint cache hits defer raw header and catalog-page copies,
the prepared primary-key point-select profile still shows avoidable storage
bookkeeping when short read statements close. The remaining dominant costs are
journal existence checks, file identity validation, advisory locks, and the
header-page read; those are safety checks. However, reusable read-statement
cleanup still calls the generic reusable-statement initializer and rewrites
every reusable statement field after the close path has already released or
cleared owned resources.

The local sample after read-checkpoint cache header-page deferral showed
`reset_reusable_read_statement_storage()` and
`initialize_reusable_statement_storage()` under the hot
`mylite_storage_end_read_statement()` path. This slice removes that generic
reset from the read-statement close path without changing lock, recovery, file
identity, or snapshot validation behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  opens a short `Mylite_read_statement_scope` for durable exact unique point
  reads.
- `packages/mylite-storage/src/storage.c::mylite_storage_begin_read_statement()`
  allocates read statements through `allocate_read_statement()`, assigns a
  filename, then runs `initialize_read_statement()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_end_read_statement()`
  closes the storage statement, then calls `free_statement()`.
- `packages/mylite-storage/src/storage.c::free_statement()` already closes or
  caches owned files, clears exact/live-row/row-payload caches, table-entry
  caches, catalog images, append buffers, undo lists, dirty-page lists, journal
  dirty pages, and buffered update rewrite caches before retaining a reusable
  read statement.
- `packages/mylite-storage/src/storage.c::reset_reusable_read_statement_storage()`
  currently preserves an owned filename, calls the generic initializer, then
  restores the filename.

## Design

- Keep the existing generic reusable initializer for newly allocated read
  statements and reusable nested checkpoint statements.
- Replace the read-statement reuse reset with a read-specific scalar reset:
  - preserve the retained owned filename pointer for same-file reuse,
  - clear borrowed filename state,
  - clear file, parent, owner, identity, lock/cache, journal, current-header,
    current-catalog, and deferred-retarget flags,
  - leave already-cleared owned cache/list structs at zero after
    `free_statement()` has released them.
- Keep shared locks, cached read-file identity validation, journal probes,
  checkpoint header reads, and read-checkpoint cache matching unchanged.

## Affected Subsystems

- First-party MyLite storage implementation in
  `packages/mylite-storage/src/storage.c`.
- Existing storage read-statement reuse tests.
- Storage-smoke performance baseline documentation.

No MariaDB upstream-derived source changes are required.

## Compatibility Impact

No SQL, public C API, metadata, or storage-engine routing behavior changes.
The opaque `mylite_storage_statement` lifetime remains unchanged. The reusable
object is still internal process-local storage.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, recovery, lock, or visibility changes.
The slice only reduces process-local cleanup work after a read statement has
released its resources.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small first-party code change. No new dependency.

## Tests And Verification

- Run `git diff --check`.
- Run `git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`.
- Build `mylite_storage_test`.
- Run storage tests covering reusable read statements, filename replacement,
  borrowed filename identity, checkpoint-cache reuse, catalog changes, and
  cached read-file path replacement.
- Rebuild the storage-smoke MariaDB archive and relink affected smoke targets.
- Run full storage-smoke CTest coverage.
- Run local `storage-read-statements` and `prepared-pk-selects` performance
  baselines.

Verification after implementation on 2026-05-22:

- `git diff --check`
- `git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --preset dev --output-on-failure -R mylite-storage`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-perf-baseline --phase=storage-read-statements 1000 10000`
  measured read-statement begin/end at `3.519 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`
  measured prepared primary-key point selects at `7.030 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 1000 1000000` measured prepared primary-key
  point selects at `7.207 us/op`.
- `/tmp/mylite_prepared_pk_after_lean_reset.sample.txt` showed the generic
  `initialize_reusable_statement_storage()` no longer under
  `mylite_storage_end_read_statement()`.

## Acceptance Criteria

- Reused read statements keep working for same-file and different-file opens.
- Borrowed filename scopes do not leak into the reusable read-statement cache.
- Catalog and checkpoint cache tests continue to observe durable changes.
- The hot close path no longer calls the generic reusable statement initializer.
- Point-read benchmarks are neutral or improved locally.

## Risks And Unresolved Questions

- This is a small cleanup. It does not reduce the safety-bound `access()`,
  `stat()`, `flock()`, or `pread()` costs that dominate short durable read
  statements.
- The specialized reset relies on `free_statement()` continuing to clear owned
  heap resources before the reusable read-statement object is retained.
