# Indexed Read Active Cache Scope

## Problem

Prepared primary-key updates first use MariaDB's handler index-read path to
find and materialize the target row, then call the update path. The update path
now resolves active cache ownership once, but the indexed-read path still
rediscovers the same active cache statement while probing exact-index caches and
active row-payload caches.

The sampled prepared-update profile before this slice showed
`find_indexed_row_payload()` under `ha_mylite::build_index_cursor()`, with
repeated `active_cache_statement_for()` and filename `strcmp()` samples under
exact-index and active row-payload cache reads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Handler source: `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()`.
- MyLite storage source: `packages/mylite-storage/src/storage.c`.
- `find_indexed_row_payload()` opens the primary file, reads the header, finds
  the table entry, calls `find_exact_index_row_id()`, and then calls
  `read_indexed_row_payload_from_open_file()` for the selected row id.
- The exact-index cache probe resolved the active exact-index cache statement
  by filename.
- The active row-payload cache probe resolved the active row-payload cache
  statement by filename.
- Exact-index, table-entry, live-row-id, and row-payload active caches share the
  same outermost owner statement for a filename and active context.

## Design

- Resolve the active cache statement once in `find_indexed_row_payload()` after
  the primary file is open.
- Pass that statement to statement-scoped exact-index and row-payload cache
  probes.
- Remove now-unused filename-based wrappers from these two probes rather than
  suppressing `-Wunused-function`.
- Preserve table-entry lookup behavior. A scoped table-entry shortcut was
  intentionally not included because an earlier attempt in the update path broke
  the embedded WordPress storage test.
- Keep active file-scoped live-row validation and marking unchanged.

## Compatibility Impact

No SQL-visible, handler API, public C API, or storage-engine routing behavior
change.

## File Lifecycle

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change. The slice only reuses an existing transient active-statement pointer
inside one storage call.

## Embedded Lifecycle And API

No public `libmylite` API or embedded lifetime change.

## Build, Size, And Dependencies

Small first-party C helper split. No new dependency and no expected material
binary-size impact.

## Test Plan

- Build storage-smoke targets with `cmake --build --preset storage-smoke-dev`.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- `find_indexed_row_payload()` avoids repeated active cache-owner lookups for
  exact-index cache probes and active row-payload cache probes.
- No unused static wrappers are left behind.
- Existing storage and embedded storage-engine coverage remains green.
- Prepared-update timing does not materially regress.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev`: passed.
- `git diff --check`: passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed,
  10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `4.233 us/op`.

## Risks And Open Questions

- This does not change table-entry cache lookup, because that shortcut needs
  separate correctness work before it is safe.
- Larger wins still require reducing row-copy and handler/MariaDB overhead or
  implementing the planned navigable-index and pager work.
