# Direct Update Integer-Key NULL Probe Skip

## Problem

Prepared row-only direct updates over an integer primary key build a key image
on every execution. The hot path first calls `Item::is_null()` when the bound
predicate value can be nullable, then calls `Item::val_int()` in the integer
key-store helper. For ordinary bound integer parameters, that duplicates
runtime value probing before the exact-key lookup.

The existing integer key-store helper already treats a `NULL` value as "no
exact key" after `val_int()` sets the `Item::null_value` flag. The eager
`is_null()` probe is still needed for non-integer key values because storing a
`NULL` into a synthetic non-null key field could raise assignment diagnostics
instead of behaving like a no-match predicate.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  calls `build_direct_update_key()` before direct exact-key lookup.
- `ha_mylite::build_direct_update_key()` eagerly probes nullable predicate
  values with `Item::is_null()`, then asks
  `mylite_store_direct_update_integer_key()` to evaluate integer values with
  `Item::val_int()`.
- `mylite_store_direct_update_integer_key()` already returns with
  `out_has_key` false when `value_item->null_value` is set after `val_int()`.
- `packages/libmylite/tests/embedded_storage_engine_test.c` already covers
  prepared exact-key updates with a bound `NULL` parameter and verifies no
  visible row change.

## Design

Compute whether the direct-update predicate item has `INT_RESULT` once in
`build_direct_update_key()`.

- For integer key values, skip the eager `maybe_null()` / `is_null()` probe and
  let the integer-store helper evaluate the value once through `val_int()`.
- For non-integer key values, keep the existing `NULL` precheck so `NULL`
  predicates do not accidentally flow through key-field assignment.
- Pass the precomputed integer-result decision to the helper so it does not
  re-read the item result type.

## Compatibility Impact

No SQL-visible behavior change is intended. A bound `NULL` exact-key predicate
still updates no rows and does not report assignment diagnostics. Non-integer
predicate values keep the existing eager `NULL` handling.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format, journal, lock, sidecar, or lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No new dependency. The change is a handler-local branch and helper signature
adjustment.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`
- `cmake --build build/mariadb-mylite-storage-smoke --target mysqlserver
  -j1`
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_embedded_statement_test
  mylite_perf_baseline -j1`
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure` passed 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 1000 100000` reported prepared
  row-only update step at 1.523 us/op.

## Acceptance Criteria

- Integer exact-key direct updates evaluate the bound key value once during key
  image construction.
- Bound `NULL` integer exact-key updates still no-op without errors.
- Non-integer key values keep their existing `NULL` precheck behavior.
- Focused storage-smoke tests pass and the prepared row-only update benchmark
  does not show an obvious regression.

## Risks And Unresolved Questions

- This is a small hot-path cleanup. It does not address the larger remaining
  SQL-layer prepared update cost in table open, DML prepare, or `JOIN` setup.
