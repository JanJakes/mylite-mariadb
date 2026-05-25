# Prepared Range Limit Result Cache

## Problem Statement

The local prepared published-leaf secondary range `LIMIT 1` benchmark remains
around 2 ms/op even after storage range cursors avoid whole static suffix
materialization. Reducing the initial storage cursor batch from 128 entries to
8 entries did not materially improve the benchmark, which points at repeated
MariaDB prepared execution as the dominant cost for hot recurring range bounds.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute()` re-enters
  MariaDB command execution for each prepared statement execution.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` builds a
  forward lower-bound cursor for supported `HA_READ_KEY_OR_NEXT` and
  `HA_READ_AFTER_KEY` shapes.
- `packages/libmylite/src/database.cc` already has a bounded one-row prepared
  result cache for simple single-parameter equality predicates. It caches only
  MariaDB-produced rows or deterministic no-row outcomes and only while the
  retained prepared read statement stays open.
- The benchmark shape is currently outside that classifier:
  `SELECT id, value FROM perf_leaf_rows FORCE INDEX (value_leaf_key)
  WHERE value >= ? ORDER BY value, id LIMIT 1`.

## Scope

- Extend the existing prepared one-row result cache classifier to accept a
  narrow identifier-only range-min shape:
  - `SELECT <identifier-list> FROM <identifier> [FORCE INDEX (<identifier>)]`
  - `WHERE <identifier> > ?` or `WHERE <identifier> >= ?`
  - `ORDER BY <identifier-list> LIMIT 1`
- Keep the existing cache storage, bound-parameter keying, read-scope lifetime,
  write invalidation, and row/miss publication rules.
- Add embedded storage-engine coverage proving repeated range-min executions
  reuse the cache and writes invalidate it.
- Record local before/after prepared range performance evidence.

## Non-Goals

- No MariaDB optimizer bypass for first execution.
- No cache for joins, expressions, functions, aliases, variable `LIMIT`, locking
  reads, reverse ranges, `ORDER BY` expressions, or multi-parameter ranges.
- No direct storage API change, handler cursor change, file-format change, or
  durable tail index.

## Design

The current `simple_single_parameter_select_sql()` classifier remains the
single gate for one-row result caching. It will keep accepting the existing
equality shape and add a second branch for range-min SQL:

1. parse identifier-only select columns;
2. parse one table identifier;
3. optionally parse `FORCE INDEX (<identifier>)`;
4. parse `WHERE <identifier> > ?` or `WHERE <identifier> >= ?`;
5. require `ORDER BY` followed by identifier references;
6. require literal `LIMIT 1`;
7. require statement end.

The result cache remains populated only after MariaDB has returned exactly one
row and the caller drains the result to `MYLITE_DONE`, or after MariaDB returns
`MYSQL_NO_DATA`. Replays are keyed by the bound parameter bytes/value and stay
inside the retained read statement. Any write closes that read scope and clears
the cache before storage visibility can change.

## Compatibility Impact

No SQL-visible behavior change is intended. MyLite replays only results that
MariaDB already produced for the same prepared statement, bound value, and
stable read snapshot. Unsupported shapes keep the normal MariaDB execution
path.

## DDL Metadata Routing Impact

No DDL metadata routing change.

## Single-File And Recovery Impact

No durable storage, journal, lock, recovery, companion-file, or file-format
change. The cache is statement-owned process memory and is cleared on retained
read-scope close.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
change.

## Build, Size, And Dependencies

No dependency or license change. Binary-size impact is limited to a few parser
helpers in first-party `libmylite` code.

## Test Plan

- Add prepared routed-storage coverage for repeated
  `WHERE score >= ? ORDER BY score, id LIMIT 1` executions.
- Assert the first execution enters the bounded storage range path and a second
  same-bound execution does not.
- Insert a new lower matching row, re-execute the same prepared statement, and
  assert the retained read-scope cache is invalidated by the write.
- Keep existing equality-cache, reset-before-drain, range continuation, and
  append-tail overlay tests passing.
- Run focused storage and embedded storage-engine tests, formatting checks, and
  the prepared published-leaf secondary range performance phases.

## Acceptance Criteria

- Repeated same-bound range-min prepared executions return the cached row inside
  the retained read scope.
- A subsequent write invalidates the cache and the same bound sees the new
  MariaDB-produced row.
- Non-matching SQL stays outside the cache classifier.
- Existing exact equality one-row cache behavior remains unchanged.
- Local prepared range `LIMIT 1` performance improves materially for recurring
  bounds, or the spec records the measured result and the slice is not committed.

## Risks And Open Questions

- This optimizes hot recurring prepared range bounds, not cold scans over unique
  bounds.
- The SQL classifier is deliberately conservative and byte-oriented. Broader
  range shapes need separate source-linked slices.

## Verification Results

Local environment: macOS worktree, `storage-smoke-dev` preset.

- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test &&
  build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test &&
  build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|embedded-storage-engine' --output-on-failure` passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-leaf-secondary-range-limit-selects 1000 10000`: prepared
  published-leaf secondary range `LIMIT` selects measured `2.287 us/op`
  locally, down from the previous local baseline of about `2093.026 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-leaf-secondary-tail-range-limit-selects 1000 10000`:
  prepared published-leaf secondary tail-overlay range `LIMIT` selects measured
  `2.933 us/op` locally, down from the previous local baseline of about
  `2749.675 us/op`.
