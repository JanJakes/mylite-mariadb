# Read-Scope Row Payload Cache Store

## Problem

Prepared primary-key point selects now pass the active read statement as the
cache owner for indexed-row payload reads. That lets storage look for an
existing row-payload cache on the read statement.

The first cache miss still stores through `store_active_row_payload()`, which
resolves a cache owner by filename through `active_statement_for()`. That path
only sees active write statements, not `active_read_statement`, so read-scope
point selects do not seed the active row-payload cache after the first row read.
The same miss path also probes the durable row-payload cache even though durable
caches are deliberately unavailable while an active read or write statement owns
the file view.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::begin_prepared_read_scope()` keeps one
  MyLite storage read statement open across fully drained prepared result
  reset/re-execute loops.
- `packages/mylite-storage/src/storage.c::find_indexed_row_payload()` now
  resolves `active_cache_statement` from either the active write statement or
  the active read statement.
- `read_indexed_row_payload_from_open_file()` accepts an active row-payload
  cache pointer and can hit it, but miss handling calls
  `store_active_row_payload()`.
- `store_active_row_payload()` uses
  `active_row_payload_cache_statement_for(filename)`, which delegates to
  `active_cache_statement_for(filename)` and only scans active write
  statements.
- `durable_row_payload_cache_available()` returns false whenever an active
  statement or active read snapshot owns the file, so durable cache lookups on
  the active read-scope path are avoidable work.

## Design

- Pass the already resolved `active_cache_statement` into
  `read_indexed_row_payload_from_open_file()`.
- On active-cache misses, probe durable row-payload caches only when no active
  cache statement owns the current file view.
- Store row payloads through a new helper that accepts the resolved statement
  directly, so read-scope point selects seed their statement-owned payload
  cache after the first materialization.
- Keep the existing filename-based `store_active_row_payload()` wrapper for
  callers that do not already have a resolved statement.

## Compatibility Impact

No SQL-visible behavior changes. The cache is transient, statement-owned, and
uses the same header/catalog/table identity checks as the existing active
row-payload cache.

## Single-File And Lifecycle Impact

No file-format, journal, lock, recovery, or companion-file change. The cache is
discarded when the read statement ends.

## Binary Size And Dependency Impact

No dependency change. Binary-size impact is limited to one small helper and a
focused test hook.

## Test And Verification Plan

- Add a storage test hook that verifies whether a statement owns a row-payload
  cache.
- Extend read-scope indexed-row coverage to assert that
  `mylite_storage_find_indexed_row_into()` seeds a row-payload cache on the
  active read statement.
- Run `git diff --check` and `git clang-format --diff`.
- Build `mylite_storage_test` and `mylite_perf_baseline`.
- Run `ctest --preset storage-smoke-dev -R mylite-storage.capabilities`.
- Run focused storage and routed prepared point-read benchmarks.

## Acceptance Criteria

- Indexed-row payload reads under an active read statement seed that statement's
  row-payload cache after a miss.
- Durable row-payload cache probes are skipped while an active cache statement
  owns the current read/write view.
- Storage capability tests pass.
- Point-read benchmarks remain correct and do not regress.

## Risks And Open Questions

- This is still a storage-side optimization. MariaDB prepared SELECT planning
  remains the dominant routed point-select cost.
- Active row-payload cache memory limits remain unchanged; larger result loops
  may still clear the active cache when the existing bounds are reached.
