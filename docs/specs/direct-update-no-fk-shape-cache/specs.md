# Direct Update No-FK Shape Cache

## Problem

Accepted prepared row-only direct updates still enter
`ha_mylite::update_row()` and ask whether the table has parent or child foreign
keys on every row. The handler already proves the hot row-only direct-update
shape does not change any indexed key image. Because MyLite foreign-key
definitions are backed by table keys, a stable-index update cannot change the
child key value or the parent referenced key value that FK enforcement depends
on.

The existing foreign-key presence cache makes the repeated checks cheap, but
they remain visible noise in the prepared row-only update profile.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows_init()`
  computes `direct_update_may_change_index_entries` from the handler write set
  and accepted table keys.
- The same initializer rejects key-changing direct updates for tables involved
  in parent or child foreign keys.
- `ha_mylite::direct_update_rows()` asserts that the accepted direct path does
  not need MariaDB private in-server update constraints, then calls
  `ha_mylite::update_row()` directly.
- `ha_mylite::update_row()` still computes `check_foreign_keys` and calls
  `has_parent_foreign_keys()` / `has_child_foreign_keys()` before deciding that
  no FK action or validation work is needed.
- MyLite foreign-key validation uses key prefixes via
  `mylite_find_foreign_key_prefix()` and the stored FK metadata. A row update
  that leaves all index entries unchanged cannot alter those FK key values.

## Design

Treat an accepted direct update with `direct_update_may_change_index_entries`
set to false as FK-stable inside `update_row()`:

- keep the existing FK checks for ordinary row updates;
- keep the existing FK checks for direct updates that may change any indexed
  key image;
- skip parent and child FK presence/action checks only for the accepted
  direct-update row-only shape.

This relies on the same shape facts already used to preserve index entries and
skip duplicate-key checks. It does not change direct-update admission.

## Compatibility Impact

No SQL-visible behavior change is intended. Updates that change FK-relevant
keys still use the existing FK checks or fall back before the direct path. Row
updates that leave all indexed key values unchanged cannot introduce a missing
child parent, change a referenced parent key, or trigger an `ON UPDATE` action.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format, journal, lock, sidecar, or lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No new dependency. The change is a handler-local branch and focused regression
coverage.

## Tests And Verification

Passed on 2026-05-23:

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  packages/libmylite/tests/embedded_storage_engine_test.c`
- `cmake --build build/mariadb-mylite-storage-smoke --target mysqlserver
  -j1`
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline -j1`
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine' --output-on-failure` passed 1/1 test.
- `ctest --preset storage-smoke-dev --output-on-failure` passed 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 1000 100000` reported prepared
  row-only update step at 1.473 us/op.

## Acceptance Criteria

- Direct row-only updates skip FK presence probes after direct admission proves
  no indexed key image can change.
- FK-involved parent and child tables still accept prepared direct updates that
  modify only non-key columns.
- Existing FK key-changing update failures remain covered by the broader
  storage-engine test suite.
- Focused storage-smoke tests pass and the prepared row-only benchmark does not
  regress.

## Risks And Unresolved Questions

- The optimization must stay tied to the already accepted direct-update shape.
  It must not skip FK checks for ordinary handler updates or for any update
  where an indexed key entry may change.
