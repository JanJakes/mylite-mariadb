# Read Checkpoint Cache Header-Page Deferral

## Problem

After durable row-payload cache hits stopped rehashing cached rows, the prepared
primary-key point-select sample still shows read-statement startup cost under
`read_cached_checkpoint_snapshot()`. The header page must still be read from
the primary file to detect external durable changes, but a hot cache hit then
copies the same cached header page into every short-lived read statement.

Most hot point-read paths only need the decoded header already stored on the
statement. Copying the raw header page is unnecessary unless a later caller asks
to read page `0` through the active read statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  starts a short MyLite read statement around each durable exact unique point
  read.
- `packages/mylite-storage/src/storage.c::read_cached_checkpoint_snapshot()`
  still reads the raw header page on every read statement and compares it with
  the thread-local read-checkpoint cache. This remains the required durable
  change detection point.
- `packages/mylite-storage/src/storage.c::copy_read_checkpoint_cache_to_statement()`
  copies the cached decoded header and the cached raw header page into the
  short-lived statement on a hot cache hit.
- `packages/mylite-storage/src/storage.c::materialize_statement_header_page()`
  already lazily reconstructs `statement->header_page` from the decoded header
  when a read-statement caller later asks for page `0`.
- Existing catalog-image and catalog-page borrow slices already defer catalog
  page copies on the same hot cache-hit path.

## Design

- On read-checkpoint cache hits, copy decoded header state into the read
  statement but leave `has_header_page` unset.
- Keep the raw header page read and byte comparison unchanged before accepting
  a cache hit.
- Keep cache misses unchanged: statements that decode a fresh snapshot from the
  raw header page still retain that page.
- Keep lazy materialization for the rare page-`0` active-read path.
- Do not change journal probes, shared locks, checkpoint cache identity checks,
  catalog-page deferral, or durable file-format behavior.

## Affected Subsystems

- First-party MyLite storage read-checkpoint cache.
- Durable handler point reads that create short storage read statements.
- Storage-smoke performance baseline.

## Compatibility Impact

No SQL, handler, public C API, metadata, or storage-engine routing behavior
changes. The decoded header copied into the statement is the same header that
was previously paired with an eager raw-page copy.

## Single-File And Lifecycle Impact

No durable file-format, lock, journal, recovery, or companion-file lifecycle
change. Read statements still read page `0`, acquire the same shared lock, and
release it at the same point.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small first-party C deletion. No new dependency.

## Tests And Verification

- Reuse storage tests covering read-checkpoint cache reuse, catalog changes,
  journal recovery, read-file replacement, and storage-engine point reads.
- Rebuild `mylite_storage_test`, the storage-smoke MariaDB archive, routed
  smoke tests, and `mylite_perf_baseline`.
- Run focused and full storage-smoke CTest coverage.
- Run storage read-statement and prepared primary-key point-select performance
  baselines.
- Run `git diff --check` and changed-file formatting checks.

Verification after implementation on 2026-05-22:

- `git diff --check`
- `git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --preset dev --output-on-failure -R mylite-storage`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-perf-baseline --phase=storage-read-statements 1000 10000`
  measured read-statement begin/end at `3.607 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`
  measured prepared primary-key point selects at `7.118 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 1000 1000000` measured prepared primary-key
  point selects at `7.292 us/op`.

## Acceptance Criteria

- Hot read-checkpoint cache hits do not copy the raw header page into every
  short-lived read statement.
- A later page-`0` read through the active read statement can still materialize
  equivalent header bytes lazily.
- Cache misses and full checkpoint snapshot validation remain unchanged.
- Existing storage and routed storage-engine tests pass.

## Risks And Unresolved Questions

- The raw header page `pread()` stays in the hot path. Removing that requires a
  broader read-view or pager design.
- This is a small allocation/copy cleanup, not a substitute for reducing
  journal probes, locks, or MariaDB optimizer work.
