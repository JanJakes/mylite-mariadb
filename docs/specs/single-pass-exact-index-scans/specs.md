# Single-Pass Exact Index Scans

## Problem

Durable exact index lookups still pay two file walks. Storage first scans all
pages to build the row-state map, then scans all pages again to find matching
index-entry pages. Recent exact-entry and exact-entryset helpers reduce handler
allocation and filtering, but the storage layer still repeats page reads before
the planned B-tree navigation work exists.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` enters exact key reads
  through `ha_index_read_map()`, and equality range continuation uses
  `handler::read_range_next()` / `ha_index_next_same()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()` routes
  guarded integer-family full-key exact reads to
  `mylite_storage_find_index_entry()` for unique keys and
  `mylite_storage_read_exact_index_entries()` for non-unique keys.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()` and
  `mylite_storage_read_exact_index_entries()` call `build_row_state_map()` before
  scanning index-entry pages. `build_row_state_map()` scans from page 2 through
  the published header page count, so exact lookups walk the append-only page
  history twice.
- Current index-entry pages are append-only records containing table id, row id,
  key number, and MariaDB key-tuple bytes. Row updates and deletes append
  row-state pages that hide older row ids.

## Design

- Add page decoders that validate already-read index-entry and row-state pages.
  Existing `read_*_page()` helpers will keep their public behavior by reading a
  page and delegating to the decoders.
- Change durable exact index lookups to read each published page once:
  - skip known non-index page types,
  - decode index-entry pages and collect matching candidates not already hidden,
  - decode row-state pages and hide or prune candidates for source row ids.
- Preserve existing append-only semantics. The scan must still read through the
  published header page count so later row-state pages can invalidate earlier
  matching index entries.
- Leave broad index reads and prefix existence checks on the existing row-state
  map path. This slice only touches the hot exact-key APIs added by the previous
  performance work.

## Compatibility Impact

SQL-visible behavior should not change. The handler guards that decide when
byte-exact storage lookup is safe remain unchanged, and the storage APIs still
filter hidden row ids before returning row ids to the handler.

## Single-File And Lifecycle Impact

No file-format, sidecar, journal, or recovery change. The optimization reads
the same append-only pages and respects the same header-published page count.

## Storage-Engine Routing Impact

The durable MyLite path changes for exact index lookups only. Runtime-volatile
MEMORY/HEAP index lookup already scans process-local row vectors and is outside
this file-read optimization.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to small page decoder and
candidate-pruning helpers in first-party storage code.

## Tests And Verification

- Keep existing storage unit coverage for live, stale, replacement, missing, and
  misuse exact-index lookup behavior passing.
- Run the storage unit test target and storage compatibility smoke that exercise
  exact primary and secondary index reads.
- Run the local performance baseline and record the result as noisy local
  evidence rather than a threshold.
- Run changed-line formatting checks and `git diff --check`.
- Local verification on this branch:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `tools/mylite-compat-harness run storage-engine`
  - `tools/mylite-perf-baseline`
  - `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `git diff --check`
- The local default performance-baseline run measured direct/prepared
  primary-key point selects at `3028.690` / `3192.050` us/op and
  direct/prepared secondary-index exact selects at `6957.790` / `9913.320`
  us/op. This remains noisy workstation evidence; it confirms the benchmark path
  still runs, not that the interim scan optimization has reached the target
  storage-navigation profile.

## Acceptance Criteria

- Durable exact index lookup and exact entryset APIs scan the append-only page
  history once per call.
- Later row-state pages invalidate earlier matching candidates before results
  are returned.
- Existing exact lookup behavior and storage-smoke SQL behavior remain covered.
- Docs continue to state that this is an interim scan optimization, not
  storage-level B-tree navigation.

## Risks

- This does not change the asymptotic behavior for exact lookups; B-tree or
  equivalent navigable index pages remain required for SQLite-like point-read
  performance.
- The one-pass scanner must preserve corruption checks for unknown page types
  instead of silently skipping malformed pages.
