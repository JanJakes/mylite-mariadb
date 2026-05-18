# Prepared Routed Select Reads

## Problem Statement

The routed-storage performance baseline raised doubt about whether prepared
statements can read rows from MyLite-routed durable tables. Direct reads already
had broad coverage, and prepared routed DML had direct-read assertions, but
there was no focused regression proving prepared `SELECT` sees the same routed
rows through full-scan and index-backed access paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc` implements prepared execution through
  `Prepared_statement` and `Select_fetch_protocol_binary`; prepared row data is
  sent through the same SQL execution pipeline, but through binary result
  protocol methods.
- `packages/libmylite/src/database.cc` maps `mylite_prepare()` and
  `mylite_step()` to MariaDB's embedded `MYSQL_STMT` APIs, binds result
  buffers in `bind_statement_results()`, and fetches rows in
  `fetch_statement_row()`.
- `mariadb/storage/mylite/ha_mylite.cc` serves handler scans through
  `rnd_init()` / `rnd_next()` and index reads through `index_read_map()` /
  `index_read_idx_map()`, so prepared and direct SQL should reach the same
  storage-engine row paths after MariaDB planning.

## Scope

- Add focused storage-smoke regression coverage for prepared `SELECT` over an
  `ENGINE=InnoDB` table routed to MyLite storage.
- Cover direct-read baselines, prepared aggregate count, prepared primary-key
  lookup, prepared secondary-index lookup, and a scalar-parameter lookup.
- Cover both text and native integer result retrieval through the public
  `libmylite` API.

## Non-Goals

- Add benchmark thresholds.
- Claim prepared execution performance parity with direct execution.
- Implement new handler access methods, B-tree navigation, cursor caching, or
  prepared statement reuse optimizations.

## Compatibility Impact

This is test coverage for existing intended behavior. MyLite's public prepared
statement API is expected to return the same rows as direct SQL for covered
routed MyLite tables. The slice does not expand supported SQL syntax or change
storage routing policy.

## Single-File And Embedded Lifecycle

The regression uses a temporary file-backed `.mylite` database and checks that
the existing durable-sidecar gate remains clean after close. It does not add new
file lifecycle artifacts.

## Test And Verification Plan

- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror` on the changed
  `embedded_storage_engine_test.c` ranges.
- `git diff --check`

## Acceptance Criteria

- Prepared routed `SELECT COUNT(*)` returns the direct row count through native
  integer retrieval.
- Prepared routed text projections return expected rows through full-scan,
  primary-key, secondary-index, and one-bound-parameter paths.
- The storage-engine compatibility group remains green.

## Risks And Open Questions

- This confirms correctness, not performance. Prepared-statement timing belongs
  in the separate `docs/specs/prepared-performance-baseline/` measurement slice.
