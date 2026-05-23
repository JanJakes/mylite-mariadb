# Direct Update Integer Key Reset Skip

## Problem

The accepted exact-key direct-update path still zeros the direct lookup key
buffer and resets the cached key field before every key build. The remaining
`prepared-row-only-update-components` sample after compact snapshot shape
caching still shows `ha_mylite::build_direct_update_key()` with visible
`bzero` / `_platform_memset` cost.

For MyLite's accepted direct-update key shape, the hot integer path stores into
a one-part, non-null, fixed-width integer-family key field. That store writes
the complete key bytes, so the generic reset is not needed before the integer
fast path. Resetting remains necessary for the fallback conversion path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_direct_update_key()`
  clears `direct_update_key_buffer`, resets the cached direct key field, then
  attempts the guarded integer key-store helper before falling back to
  `Item::save_in_field()`.
- `mylite_direct_update_key_is_supported()` only accepts one user-defined key
  part, no nullable key parts, matching key field length/store length, no BLOB
  or variable-length key part, and a raw exact-filter integer-family field.
- `mylite_key_uses_raw_exact_filter()` accepts `TINY`, `SHORT`, `INT24`,
  `LONG`, `LONGLONG`, and `YEAR` fields for this key path.
- MariaDB `Field_tiny::store(longlong, bool)`,
  `Field_short::store(longlong, bool)`, `Field_medium::store(longlong, bool)`,
  `Field_long::store(longlong, bool)`,
  `Field_longlong::store(longlong, bool)`, and
  `Field_year::store(longlong, bool)` write their complete fixed-width field
  payloads.

## Design

- Remove the unconditional key-buffer clear from `build_direct_update_key()`.
- Keep `direct_update_key_null` reset before each key build.
- Attempt the guarded integer store before calling `Field::reset()`.
- When the integer helper handles the value:
  - treat bound `NULL` values as no-key/no-match using the existing
    `value_item->null_value` check,
  - rely on the fixed-width field store to write the complete key image,
  - preserve warnings and conversion errors from `Field::store()`.
- When the integer helper does not handle the value, reset the key field before
  the existing generic `Item::save_in_field()` fallback.

## Affected Subsystems

- MyLite MariaDB storage handler direct-update key construction.
- Prepared row-only update performance evidence.

## Compatibility Impact

No SQL-visible behavior change is intended. Supported integer direct-update
predicates keep the same lookup bytes, null no-match behavior, warnings,
errors, affected rows, and rollback behavior. Non-integer predicate values keep
the generic MariaDB conversion path with the key-field reset.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, recovery, sidecar, or embedded-lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. The rebuilt storage-smoke embedded archive measured
`21274424` bytes.

## Test And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc
  packages/libmylite/tests/embedded_storage_engine_test.c`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_embedded_statement_test
  mylite_perf_baseline`.
- Extended prepared primary-key update coverage with a text-bound key predicate
  so the generic conversion fallback remains covered.
- Passed focused embedded statement and storage-engine tests with:
  `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.585 us/op`
  - reset: `0.022 us/op`
- Ran `prepared-row-only-update-components` with
  `--profile-iterations=20000000`:
  - bind: `0.022 us/op`
  - step: `1.581 us/op`
  - reset: `0.023 us/op`
- Ran sampled `prepared-row-only-update-components` with
  `--profile-iterations=20000000` and macOS `sample`:
  - sample file: `/tmp/mylite-direct-update-integer-key-reset-skip.sample.txt`
  - the accepted integer key path no longer shows key-buffer/reset zeroing below
    `ha_mylite::build_direct_update_key()`. Remaining key-build samples are in
    item integer value access and `Field::store()`.

## Acceptance Criteria

- Supported integer exact-key direct updates skip generic key-field reset.
- Bound `NULL` integer exact-key updates still no-op without errors.
- Non-integer exact-key predicate values keep their existing reset plus generic
  conversion behavior.
- Existing storage-smoke tests pass.
- Local performance evidence shows no material regression and the hot sample
  removes the redundant key reset/clear cost from the accepted branch.

## Risks

- Skipping reset is safe only because the accepted direct-update key shape is
  fixed-width and the integer store writes all key bytes. This slice must not
  broaden direct-update key support to strings, decimals, nullable keys,
  variable-length fields, or multi-part keys.
