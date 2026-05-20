# Write-Set Key Change Filter

## Problem

`ha_mylite::update_row()` computes changed index entries by rebuilding the old
key image for every supported index and comparing it with the newly prepared
key image. The prepared update benchmark updates only a secondary integer key,
but the handler still rebuilds and compares the stable primary-key image on
every row update.

That repeated `key_copy()` work is visible after removing per-row handler heap
allocation from small index-entry preparation.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` calls
  `TABLE::mark_columns_needed_for_update()` before row updates.
- `mariadb/sql/table.cc::TABLE::mark_columns_needed_for_update()` preserves
  `TABLE::write_set` as the set of fields written by the update and marks
  generated/stored virtual columns when written base columns can affect them.
- `mariadb/sql/table.cc::TABLE::mark_virtual_columns_for_write()` marks stored
  generated columns, and generated columns that participate in key expressions,
  when their dependencies are written.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_prepare_index_entry_changes()`
  currently calls `key_copy()` for every index before deciding whether each key
  image changed.

## Design

Add a private handler helper that answers whether an index key may have changed
from MariaDB's `TABLE::write_set`:

- If the table, write set, key descriptor, or key-part field metadata is
  missing, return "may change".
- If any user-defined key-part field is in `write_set`, return "may change".
- Otherwise return "unchanged".

Use that helper inside `mylite_prepare_index_entry_changes()` after validating
the new entry descriptor but before rebuilding the old key image. When the key
cannot have changed, mark that entry unchanged and skip old-key `key_copy()`.

This is a filter only. It never marks a key changed, and it falls back to the
existing byte-image comparison whenever metadata is incomplete or any key-part
field may have changed.

MyLite also mutates `new_data` inside the handler for same-row self-referencing
foreign-key actions. Those changes happen after MariaDB builds `TABLE::write_set`,
so the handler must mark the affected child key-prefix fields as written before
the changed-key filter runs.

## Compatibility Impact

No SQL-visible behavior changes. The optimization relies on MariaDB's own
update column bitmap after virtual/generated dependency marking. Unsupported or
unknown key metadata keeps the existing byte comparison path. Handler-owned
same-row foreign-key rewrites mark their locally changed key columns so index
maintenance still sees those keys as possibly changed.

## File And API Impact

No public API, storage C API, file-format, or companion-file change.

## Storage Routing Impact

No engine routing change. The optimization applies inside the MyLite handler
after MariaDB routes a row update to MyLite storage.

## Binary-Size Impact

Negligible private helper code in the handler.

## Test And Verification Plan

- Build the storage-smoke MariaDB archive and focused MyLite smoke binaries.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample the benchmark and confirm old-key `key_copy()` no longer appears for
  the stable primary key in the benchmark update path.

## Acceptance Criteria

- The WordPress/application-schema smoke remains green.
- Update/delete/FK/transaction smoke coverage remains green.
- Stable-key updates keep unchanged index-entry masks without rebuilding old
  key images for indexes whose key-part fields are not written.
- Any uncertain metadata path keeps the existing full key-image comparison.

## Verification

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC` passed, producing a 20.08 MiB embedded
  archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_embedded_storage_engine_test` passed.
- `git diff --check` passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed:
  10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` reported prepared primary-key updates
  at 4.761 us/op. Additional runs reported 4.473, 4.641, and 4.630 us/op.
- Sampling confirmed `key_copy()` remains on the benchmark path for the
  intentionally changed `value_key(value)` secondary index. The primary-key
  old-image comparison is skipped by the write-set filter.

## Risks

- A false unchanged result would corrupt index maintenance. The helper must
  therefore be conservative and use only MariaDB-maintained `write_set` data
  after generated-column dependency marking.
