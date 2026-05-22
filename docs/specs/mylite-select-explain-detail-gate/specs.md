# MyLite Select Explain Detail Gate

## Problem

Prepared primary-key point selects still spend visible time in MariaDB's
ordinary `JOIN::save_explain_data()` path even when no explicit `EXPLAIN`,
`ANALYZE`, or slow-log explain output is requested. The hot work builds plan
details such as table names, possible keys, quick-select text, ref lists, and
extra strings that are useful for plan output but not exposed through MyLite's
ordinary embedded statement APIs.

The existing MyLite update/delete explain-detail gate already avoids similar
ordinary-execution detail allocation for routed `UPDATE` and `DELETE` while
preserving explicit explain behavior. This slice applies the same principle to
ordinary MyLite `SELECT` plan details without removing execution trackers that
MariaDB uses while running the statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_select.cc::JOIN::build_explain()` runs after SELECT
  optimization and calls `JOIN::save_explain_data()`.
- `mariadb/sql/sql_select.cc::JOIN_TAB::save_explain_data()` assigns runtime
  trackers such as `JOIN_TAB::tracker`, `jbuf_tracker`, and filesort trackers,
  then builds user-visible explain details.
- SELECT execution increments those trackers from `sub_select()` and join
  cache code, so ordinary MyLite execution must keep tracker setup.
- `mariadb/sql/sql_select.cc::JOIN_TAB::build_range_rowid_filter()` expects a
  rowid-filter tracker when rowid filtering is used.
- `mariadb/sql/sql_delete.cc::Update_plan::save_explain_data_intern()` already
  gates update/delete detail allocation behind explicit `EXPLAIN`, `ANALYZE`,
  or slow-log explain/engine verbosity when MyLite schema hooks are active.
- `packages/libmylite/tests/embedded_storage_engine_test.c` already covers an
  explicit routed `EXPLAIN SELECT` key choice for a secondary-index plan.

## Design

- Include MyLite schema-hook visibility in `sql_select.cc`.
- In `JOIN_TAB::save_explain_data()`, compute whether full explain details are
  needed:
  - non-MyLite execution,
  - explicit `EXPLAIN`,
  - `ANALYZE`,
  - slow-log explain or engine verbosity.
- Keep the existing tracker setup for all executions:
  - pre-join filesort tracker allocation,
  - table/join-buffer tracker pointers,
  - handler time tracker setup for `ANALYZE` or slow-log engine verbosity,
  - rowid-filter tracker setup when a rowid filter exists.
- For ordinary MyLite SELECT execution that does not need explain details,
  return after tracker setup and skip table-name strings, possible keys,
  quick-info allocation, ref-list construction, MRR text, BKA explain data, and
  other output-only fields.
- Preserve the existing full path for explicit `EXPLAIN SELECT`, `ANALYZE`,
  slow-log explain/engine detail, and non-MyLite MariaDB behavior.

## Scope

In scope:

- Ordinary MyLite `SELECT` detail allocation under `JOIN_TAB::save_explain_data()`.
- Existing routed `EXPLAIN SELECT` coverage.
- Prepared primary-key SELECT performance evidence.

Out of scope:

- Changing range optimization, join-order search, or prepared-plan reuse.
- Removing read-statement safety checks such as journal probes, file identity
  checks, shared locks, or header-page reads.
- Supporting `SHOW EXPLAIN` as a MyLite embedded API surface.

## Compatibility Impact

Explicit routed `EXPLAIN SELECT`, `ANALYZE`, and slow-log explain/engine detail
stay on the full upstream path. Non-MyLite MariaDB behavior is unchanged. The
optimized path only applies to ordinary MyLite execution where the detailed
explain tree is not surfaced by supported embedded APIs.

## Single-File And Lifecycle Impact

No durable storage, companion-file, lock, recovery, or file-format change.

## Public API And File-Format Impact

No public MyLite API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small upstream-derived MariaDB patch. No new dependency.

## Tests And Verification

- Reuse routed storage-engine coverage that `EXPLAIN SELECT ... WHERE
  secondary_key =` reports the expected key.
- Build the storage-smoke MariaDB archive and relink storage-smoke targets.
- Run full storage-smoke CTest coverage.
- Run `git diff --check`.
- Run formatting checks over changed files.
- Run `prepared-pk-selects` performance phases and sample the hot path to
  confirm ordinary MyLite SELECT no longer samples the skipped detail work.

Verification after implementation on 2026-05-22:

- `git diff --check`
- `git-clang-format --diff HEAD -- mariadb/sql/sql_select.cc`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`
  measured prepared primary-key point selects at `7.318 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-scalar-selects 1000 10000`
  measured prepared scalar selects at `0.754 us/op`.
- Repeated unsampled `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 1000 1000000` runs measured prepared
  primary-key point selects at `7.279 us/op` and `7.218 us/op`.
- `/tmp/mylite_prepared_pk_select_gate.sample.txt` showed ordinary MyLite
  point selects no longer sampling `JOIN_TAB::save_explain_data()` detail work
  such as quick-info, possible-key, ref-list, or MRR explain construction.

## Acceptance Criteria

- Explicit routed `EXPLAIN SELECT` still reports the expected secondary key.
- Ordinary MyLite prepared primary-key point selects keep executing correctly.
- Ordinary MyLite SELECT execution keeps required runtime trackers initialized.
- Non-MyLite behavior remains unchanged.
- Prepared point-select benchmarks are neutral or improved locally.

## Risks And Unresolved Questions

- This does not remove MariaDB join optimization itself; it only avoids
  ordinary-execution explain detail construction after optimization.
- SHOW EXPLAIN is not a supported MyLite embedded surface. If that changes, the
  skipped ordinary-execution details will need a separate design.
