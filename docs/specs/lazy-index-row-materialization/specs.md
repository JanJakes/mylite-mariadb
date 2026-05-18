# Lazy Index Row Materialization

## Problem Statement

Routed primary-key point reads and updates are currently much slower than they
should be for small benchmark tables. The handler builds an in-memory index
cursor by reading all live index entries, reading every referenced row once to
measure BLOB payload needs, reading every row a second time to materialize row
buffers, and sorting the entries. A point lookup then returns only one of those
materialized rows. This is a direct cost multiplier before the planned B-tree
storage work even starts.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Before this slice,
  `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` read all index
  entries through `mylite_storage_read_index_entries()` or
  `mylite_volatile_read_index_entries()`, then eagerly read every row payload
  before sorting.
- Before this slice,
  `mariadb/storage/mylite/ha_mylite.cc::read_index_cursor_row()` copied a
  pre-materialized row from `index_rows`.
- `mariadb/storage/mylite/ha_mylite.cc::rnd_pos()` already shows the safe
  row-id-to-record-buffer path: read the row payload, scan BLOB metadata, copy
  the row into MariaDB's record buffer, and retain record BLOB payloads for the
  current buffer slot.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  filters hidden/deleted row ids before returning an entryset, so the handler
  does not need eager row reads to filter stale durable entries.

## Scope

- Keep the current append-only index-entry storage format and sorted in-memory
  cursor.
- Stop materializing all row buffers during `build_index_cursor()`.
- Store only key offsets, key sizes, and row ids in the cursor.
- Materialize the selected row lazily in `read_index_cursor_row()` by row id.
- Preserve BLOB/TEXT row handling and MariaDB record-buffer lifetime behavior.

## Non-Goals

- Implement B-tree navigation or physical index pages.
- Avoid scanning index-entry pages.
- Cache cursors across statements.
- Add benchmark thresholds or product performance claims.

## Compatibility Impact

The handler must preserve the same supported index semantics: exact lookups,
range iteration through `index_next()` / `index_prev()`, ordered first/last
reads, duplicate checks that depend on stored index entries, BLOB/TEXT prefix
indexes, and volatile MEMORY/HEAP index reads.

## Single-File And Embedded Lifecycle

No file-format changes. Durable data remains in the primary `.mylite` file, and
the slice only changes when the handler reads row payloads.

## Test And Verification Plan

- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `tools/mylite-perf-baseline`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror --lines=1147:1156 --lines=1188:1194 --lines=1265:1307 --lines=1317:1378 mariadb/storage/mylite/ha_mylite.cc`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror --lines=39:41 --lines=52:52 --lines=63:63 mariadb/storage/mylite/ha_mylite.h`
- `git diff --check`

## Acceptance Criteria

- Existing storage-engine index, BLOB/TEXT, MEMORY/HEAP, transaction, and
  application-schema smoke coverage remains green.
- The performance baseline still runs and reports checksums/row counts.
- Point-select/update timings should not regress from the previous local
  baseline unless evidence shows the workload shifted.

## Implementation Evidence

The implemented handler cursor stores sorted key metadata and row ids only.
`read_index_cursor_row()` reads the selected durable or volatile row payload by
row id, scans BLOB/TEXT payload needs, reconstructs MariaDB's record buffer, and
retains per-record BLOB payload storage using the same record-buffer lifetime
model as `rnd_pos()`.

Local before/after performance evidence on the same machine:

| Operation | Previous us/op | Current us/op |
| --- | ---: | ---: |
| direct primary-key point selects | 51250.010 | 3718.770 |
| prepared primary-key point selects | 58038.540 | 3638.330 |
| direct primary-key updates in one transaction | 144922.240 | 10786.750 |
| prepared primary-key updates in one transaction | 342595.610 | 18786.840 |

## Risks And Open Questions

- The cursor still scans all index entries and sorts in memory. This is an
  incremental reduction in row materialization cost, not the final indexed
  storage design.
- Range scans now read row payloads on demand, which is better for short ranges
  but may trade memory for repeated storage reads on long scans. The ordered
  scan row in the baseline remains direct table-scan based, so broader range
  benchmarks are still needed.
