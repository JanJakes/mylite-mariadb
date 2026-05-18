# MTR Temporal, Autoincrement, And Optimizer Smoke

## Goal

Promote a small set of upstream MariaDB MTR cases that pass cleanly in the
MyLite embedded MTR smoke profile and broaden coverage for temporal edge
expressions, autoincrement edge cases, charset conversion expressions, and
optimizer metadata.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/temporal_scale_4283.test` covers temporal
  arithmetic scale edge cases around `TIMESTAMPADD()` and `CONVERT_TZ()`.
- `mariadb/mysql-test/main/second_frac-9175.test` covers `MICROSECOND`
  parsing through `TIMESTAMPDIFF()`, `EXPLAIN EXTENDED`, and a simple view.
- `mariadb/mysql-test/main/mdev316.test` covers charset conversion and
  comparison expressions involving `CONVERT(... USING ...)`, `IN`, `CASE`,
  numeric casts, and date casts.
- `mariadb/mysql-test/main/insert_update_autoinc-7150.test` covers
  `INSERT ... ON DUPLICATE KEY UPDATE` when inserted rows include `NULL` for an
  autoincrement column.
- `mariadb/mysql-test/main/strict_autoinc_3heap.test` covers strict
  autoincrement behavior through the upstream HEAP/MEMORY include.
- `mariadb/mysql-test/main/optimizer_costs2.test` covers default
  `INFORMATION_SCHEMA.OPTIMIZER_COSTS` and memory optimizer cost variables.
- Probe command:
  `tools/mylite-mtr-harness probe main.temporal_scale_4283 main.second_frac-9175 main.mdev316`
  passed all three selected candidates.
- Probe command:
  `tools/mylite-mtr-harness probe main.insert_update_autoinc-7150 main.optimizer_costs2 main.ctype_utf16_def main.strict_autoinc_3heap`
  passed the three promoted candidates and rejected `main.ctype_utf16_def`
  because the embedded profile reports an empty `ft_stopword_file` value rather
  than upstream's expected `(built-in)`.
- Earlier probes in the same discovery pass rejected broader scalar/query
  candidates that depend on omitted MyLite surfaces such as `SEQUENCE`,
  explicit native MyISAM, named locks, XML/GIS helpers, `BENCHMARK()` /
  `SLEEP()`, or omitted bootstrap tables.

## Scope

- Add these passing MTR cases to the curated embedded smoke list:
  - `main.temporal_scale_4283`;
  - `main.second_frac-9175`;
  - `main.mdev316`;
  - `main.insert_update_autoinc-7150`;
  - `main.strict_autoinc_3heap`; and
  - `main.optimizer_costs2`.
- Update the compatibility and roadmap docs to describe the new MTR coverage.

## Non-Goals

- Enabling skipped MTR tests that require the SEQUENCE engine.
- Normalizing tests that depend on omitted server surfaces or explicit native
  MyISAM.
- Changing MyLite runtime behavior or size profile.
- Treating skipped or result-drift probes as coverage.

## Compatibility Impact

The curated MTR smoke runner gains additional upstream coverage for temporal
edge expressions, view-backed `TIMESTAMPDIFF(MICROSECOND)` parsing, charset
conversion comparisons, autoincrement ODKU with `NULL` inserted values, strict
HEAP/MEMORY autoincrement behavior, and optimizer-cost metadata.

The claim remains a smoke signal. Broader MTR-scale comparison, result
normalization, and unsupported-surface mapping remain planned.

## Design

No production change is required. The harness already supports exact test
selection and suite-batched strict runs; this slice only extends the curated
default list with tests that probe confirmed as `[ pass ]`.

## File Lifecycle

No file-format or companion-file change is introduced.

## Embedded Lifecycle And API

No `libmylite` API change is required.

## Storage-Engine Routing

The selected MTR cases run under the MTR smoke profile's embedded runtime and
default Aria engine setting. They do not claim native MyISAM/InnoDB behavior.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Run the selected MTR cases through `tools/mylite-mtr-harness run`.
- Run the full curated MTR smoke list after adding the cases.
- Verify the documented curated list matches `tools/mylite-mtr-harness list`.
- Run shell syntax checks, reject-file cleanup checks, and `git diff --check`.

## Acceptance Criteria

- All six promoted tests report MTR `[ pass ]`.
- The full curated MTR smoke list remains green.
- `docs/specs/mtr-smoke-harness/specs.md` and `tools/mylite-mtr-harness list`
  stay in exact agreement.
- Compatibility and roadmap docs mention the added temporal, autoincrement,
  optimizer-cost, and charset-conversion smoke coverage.

## Risks And Open Questions

- The failed probe set reinforces that broad MTR promotion still needs explicit
  unsupported-surface mapping before larger batches are safe to add.
