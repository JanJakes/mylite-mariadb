# Direct Update Integer Key Store

## Problem

Prepared exact-key updates over integer primary keys still build the handler
lookup key through the generic `Item::save_in_field()` path on every execution.
The storage mutation path is already sub-microsecond locally, so repeated
generic item dispatch inside the accepted MyLite direct-update shape remains a
small but visible cost.

This slice keeps the MariaDB SQL prepare/open/lock path unchanged and narrows
only the handler key serialization step after MariaDB has already accepted the
direct-update proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  calls `build_direct_update_key()` before the exact unique-index lookup.
- `ha_mylite::build_direct_update_key()` creates a key `Field` over
  `direct_update_key_buffer`, then calls `Item::save_in_field()` to store the
  constant or bound predicate value into that key field.
- `mariadb/sql/item.cc::Item::save_in_field()` dispatches through the item type
  handler. For integer results, `Type_handler_int_result::Item_save_in_field()`
  calls `Item::save_int_in_field()`, which reads `val_int()`, handles `NULL`,
  and calls `Field::store(longlong, bool)`.
- MyLite's direct-update proof already limits the key shape through
  `mylite_direct_update_key_is_supported()` to one non-nullable, single-part,
  fixed-width integer-compatible unique key.

## Design

Add a guarded integer-result fast path inside `build_direct_update_key()`:

- use it only when the accepted key predicate value reports `INT_RESULT`;
- call `val_int()`, preserve `NULL` as a no-key/no-match result, and store the
  integer directly through the key field's `Field::store(longlong, bool)`;
- leave the existing `Item::save_in_field()` fallback for string, real,
  decimal, temporal, NULL-only, and any other value shape;
- keep the existing `THD::count_cuted_fields`, write-set, relaxed-copy, warning,
  and error checks around the store.

The fast path does not change direct-update eligibility, metadata validation,
table locking, expression evaluation, generated columns, CHECK constraints,
foreign keys, affected rows, or rollback behavior.

## Affected Subsystems

- MyLite storage handler direct-update execution in
  `mariadb/storage/mylite/ha_mylite.cc`.
- Prepared row-DML performance for exact integer key predicates.

## Compatibility Impact

No SQL-visible behavior change is intended. Integer bound parameters and integer
constant predicates take a shorter store path with the same `val_int()` and
`Field::store()` operations used by MariaDB's integer item save path. Non-integer
predicate values continue through MariaDB's generic conversion path.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format, durable sidecar, transaction, or embedded-lifecycle
change.

## Public API And File-Format Impact

No public `libmylite` API or file-format change.

## Binary-Size And Dependency Impact

No new dependency. The change is a small handler-local branch.

## Tests And Verification

- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`
- `git diff --check`
- `cmake --build build/mariadb-mylite-storage-smoke --target libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_embedded_storage_engine_test mylite_embedded_statement_test`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-miss-components 10000 1000000`

Current verification:

- Passed `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`.
- Passed `git diff --check`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_perf_baseline mylite_embedded_storage_engine_test
  mylite_embedded_statement_test`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `prepared-row-only-update-components 10000 1000000`: bind
  `0.022 us/op`, step `1.668 us/op`, reset `0.022 us/op`, checksum
  `1000000`.
- Ran `prepared-row-only-update-miss-components 10000 1000000`: bind
  `0.022 us/op`, step `1.138 us/op`, reset `0.021 us/op`, checksum `0`.

## Acceptance Criteria

- Prepared exact-key integer updates still pass existing routed-storage
  statement coverage, including integer rebinds, `NULL` no-match, unchanged-row,
  ignored update, transaction, FK, CHECK, and rollback cases.
- Non-integer predicate bindings remain on the generic MariaDB conversion path.
- The focused prepared row-only component benchmark improves or remains neutral;
  if timing is noisy, samples should at least show less generic item-save work
  below `ha_mylite::build_direct_update_key()`.

## Risks And Unresolved Questions

- This does not remove the dominant SQL-layer `open_tables_for_query()`,
  `JOIN::prepare()`, and `lock_tables()` cost. That remains the separate
  prepared-DML execution-reuse design.
- The fast path is deliberately limited to integer result items. Extending it to
  strings or decimals would need compatibility-specific conversion coverage.
