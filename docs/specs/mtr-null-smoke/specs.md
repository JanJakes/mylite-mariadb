# MTR NULL Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.null`. This adds curated
embedded baseline coverage for selected NULL, `IFNULL()`, `NULLIF()`,
NULL-sensitive type metadata, `IS NULL` predicates, and related optimizer
behavior.

## Non-Goals

- Broad NULL-related MTR promotion.
- Running the test through MyLite routed storage.
- Enabling native MyISAM in the embedded profile.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/null.test` covers NULL propagation through scalar
  functions and operators, NULL comparison predicates, indexed NULL lookups,
  `IFNULL()` and `NULLIF()` metadata inference, enum/set default handling, and
  selected `IS NULL` optimizer plans.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  scratch MTR tables with Aria. The initial probe failed at the first explicit
  non-semantic `ENGINE=MyISAM` scratch table.
- After replacing successful scratch MyISAM tables with Aria, the only observed
  drift was Aria/MyISAM output text and two optimizer row estimates in
  `EXPLAIN` output. The result counts and covered NULL semantics matched.

## Design

- Change successful explicit MyISAM scratch tables to Aria where the engine is
  not the tested behavior.
- Keep expected upstream result text stable with local `--replace_result`
  normalization for Aria/MyISAM emitted SQL and `SHOW CREATE TABLE` output.
- Normalize only the two known engine-dependent `EXPLAIN` row estimates with
  `--replace_column`.
- Add `main.null` to the curated MTR smoke list near the other type and
  expression tests.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected MariaDB baseline coverage
for NULL and NULLIF expression behavior. This is not routed-storage evidence
and does not imply native MyISAM support in the embedded profile.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The test runs in the MTR smoke vardir and
uses the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The storage-routed MyLite tests remain the authority for
MyLite handler behavior.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.null`
- `tools/mylite-mtr-harness run main.null`
- `tools/mylite-mtr-harness list`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.null` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.null`.
- Upstream test-source changes are limited to profile-specific normalization.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.null`: passed.
- `tools/mylite-mtr-harness run main.null`: passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 184
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `192`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test remains baseline-only. Future routed-storage NULL/NULLIF behavior
should be covered through MyLite-owned storage tests or a dedicated routed MTR
slice instead of treating this baseline smoke as storage-engine proof.
