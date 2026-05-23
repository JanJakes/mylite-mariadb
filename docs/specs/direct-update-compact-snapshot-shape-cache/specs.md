# Direct Update Compact Snapshot Shape Cache

## Problem

The accepted MyLite exact-key direct-update path could avoid copying the full
old row for stable-index row-only updates, but still walked the
`direct_update_fields` item list on every execution to prove and capture the
compact old-value snapshot shape. The starting sampled
`prepared-row-only-update-components` run over 10000 rows and 20000000
iterations still shows a small amount of time in the compact snapshot capture
helper after full-row `store_record()`, `compare_record()`, and the nested
`ha_mylite::update_row()` path have been removed from the accepted branch.

The field shape of a prepared row-only update is stable across executions when
the table share and write-set match. The direct-update shape cache already uses
those guards to reuse key-change and duplicate-check decisions, so the compact
snapshot field shape can use the same cache without broadening accepted SQL
shapes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows_init()`
  computes stable per-update-shape metadata and stores it in the existing
  direct-update shape cache when the updated fields do not require FK parent or
  child checks.
- `ha_mylite::use_direct_update_shape_cache()` validates the cache with the
  current `TABLE_SHARE`, key count, `table->write_set` shape, and cached
  write-set bitmap before reusing the cached values.
- `ha_mylite::direct_update_rows()` currently proves the compact old-row
  snapshot shape by walking `direct_update_fields`, checking that every updated
  field is stored, non-null, fixed-width, integer-compatible, and inside
  `record[0]`, then copying those old bytes before `fill_record()`.
- `mariadb/sql/field.h::Field::field_index` is the field number in the
  `TABLE::field` array. Storing field indexes instead of `Field *` pointers
  avoids reusing stale table-object pointers if a later prepared execution
  reopens the same table share with a different `TABLE` object.

## Design

- Keep the existing full-row fallback for any unsupported or uncertain shape.
- During `direct_update_rows_init()`, compute a compact snapshot shape for the
  same narrow field subset used by the current per-row capture path:
  - every explicitly updated field must resolve to a stored field,
  - nullable, virtual, out-of-record, zero-length, BLOB/TEXT, string, decimal,
    temporal, and non-integer fields remain unsupported,
  - the total captured old bytes must fit the existing small inline snapshot
    buffer.
- Store compact snapshot field indexes and byte lengths in the handler's
  current direct-update state.
- Store and restore those field indexes and byte lengths through the existing
  direct-update shape cache after the cache validates the current table share,
  key count, and write-set bitmap.
- Revalidate cached compact snapshot shapes against the current `TABLE` object
  when a cached direct-update shape is restored, then keep row execution on the
  cached field indexes and lengths.
- In `direct_update_rows()`, capture old values by iterating the prepared field
  index list rather than walking the `direct_update_fields` item list. Compare
  those cached fields after `fill_record()` to preserve no-op affected-row
  behavior.

## Affected Subsystems

- MyLite MariaDB storage handler direct-update execution.
- MyLite prepared row-only update performance evidence.

## Compatibility Impact

No SQL-visible behavior change is intended. The cache stores only the field
shape already accepted by the previous compact snapshot proof and stays guarded
by the existing direct-update shape cache validation. Unsupported shapes
continue to copy `record[1]` and use MariaDB's generic `compare_record()` path.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, recovery, sidecar, or embedded-lifecycle change.
The change is handler scratch metadata only.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. The rebuilt storage-smoke embedded archive measured
`21274488` bytes.

## Test And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_embedded_statement_test
  mylite_perf_baseline`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.620 us/op`
  - reset: `0.022 us/op`
- Ran sampled `prepared-row-only-update-components` with
  `--profile-iterations=20000000` and macOS `sample`:
  - bind: `0.021 us/op`
  - step: `1.533 us/op`
  - reset: `0.022 us/op`
  - sample file:
    `/tmp/mylite-direct-update-compact-snapshot-shape-cache-final.sample.txt`
  - the accepted compact snapshot branch no longer shows the update-field item
    list walk, `store_record(table, record[1])`, `compare_record()`, or
    `ha_mylite::update_row()`. Cached shape validation appears under
    `ha_mylite::direct_update_rows_init()`, while row execution copies and
    compares cached field bytes.

## Acceptance Criteria

- Eligible compact-snapshot direct updates reuse cached field indexes and byte
  lengths across prepared executions.
- Changed and unchanged affected-row behavior remains covered by existing
  prepared primary-key update tests.
- Unsupported compact snapshot shapes continue through the full-row fallback.
- Existing storage-smoke tests pass.
- Local performance evidence shows no material regression and the hot sample
  removes the per-row update-field list walk from the compact snapshot branch.

## Risks

- Reusing raw `Field *` pointers across prepared executions would be unsafe if
  the `TABLE` object changes. The design stores `Field::field_index` values and
  resolves them against the current `TABLE` instead.
- The cached shape must not be keyed only by the direct predicate key. It must
  remain guarded by the table write-set so different update statements do not
  share incompatible compact snapshot metadata.
