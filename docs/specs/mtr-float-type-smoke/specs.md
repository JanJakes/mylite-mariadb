# MTR Float Type Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.type_float`. This adds
curated embedded baseline coverage for selected `FLOAT` / `DOUBLE` conversion,
comparison, rounding, metadata inference, `CREATE TABLE ... SELECT`, and strict
mode edge behavior.

## Non-Goals

- Broad floating-point MTR promotion.
- Running the test through MyLite routed storage.
- Enabling native MyISAM in the embedded profile.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_float.test` exercises float and double
  insertion, conversion to integer and character columns, comparison of integer
  keys with float literals, decimal/float result type inference, wrong-scale
  diagnostics, union precision, strict out-of-range behavior, and selected
  `SHOW CREATE TABLE` metadata.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  scratch MTR tables with Aria. The only failing profile dependency observed
  before this slice was an explicit `ENGINE=MyISAM` table used for integer-key
  versus float-literal comparison behavior, not MyISAM-specific storage
  behavior.
- Existing curated MTR tests already normalize Aria/MyISAM `SHOW CREATE TABLE`
  text where the embedded profile substitutes Aria for upstream's default
  MyISAM expectation.

## Design

- Change the one explicit non-semantic `ENGINE=MyISAM` scratch table in
  `main.type_float` to `ENGINE=Aria`.
- Add local `--replace_result` directives around engine-sensitive emitted SQL
  and `SHOW CREATE TABLE` output so the upstream result expectation remains
  stable under the MyLite MTR smoke profile.
- Add `main.type_float` to the curated `tools/mylite-mtr-harness` default list
  beside the other type-behavior tests.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected MariaDB baseline coverage
for float/double type behavior. This is not a routed-storage claim and does not
broaden the supported production SQL surface by itself.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The test runs in the MTR smoke vardir and
uses the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Test Plan

- `tools/mylite-mtr-harness probe main.type_float`
- `tools/mylite-mtr-harness run main.type_float`
- `tools/mylite-mtr-harness list`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.type_float` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.type_float`.
- Any upstream test-source changes are limited to profile-specific engine
  normalization.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.type_float`: passed.
- `tools/mylite-mtr-harness run main.type_float`: passed.
- `tools/mylite-mtr-harness list`: includes `main.type_float` between
  `main.type_num` and `main.type_uint`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test remains baseline-only. Future routed-storage floating-point coverage
should use MyLite-owned storage-routed tests or a dedicated routed MTR slice
rather than treating this baseline smoke as storage-engine proof.
