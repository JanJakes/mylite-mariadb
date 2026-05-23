# MTR NULL-Key Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.null_key`. This adds
curated embedded baseline coverage for indexed `NULL` lookups, `ref_or_null`
plans, NULL-safe comparisons, outer-join NULL-key access, and selected
IS-NULL update/delete behavior.

## Non-Goals

- Broad optimizer MTR promotion.
- Running the test through MyLite routed storage.
- Enabling native MyISAM in the embedded profile.
- Restoring disabled status-variable producers in the embedded profile.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/null_key.test` covers optimizer access paths for
  indexed nullable keys, `ref_or_null`, `<=> NULL`, selected `IS NULL`
  update/delete cases, and NULL-key behavior under joins and outer joins.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  scratch MTR tables with Aria. The initial probe failed at the first explicit
  `ENGINE=MyISAM` scratch table.
- After replacing successful MyISAM scratch tables with Aria, the remaining
  observed drift was the final `SHOW STATUS LIKE "handler_read%"` block. The
  current embedded smoke profile compiles out status metadata producers, so the
  statement returns only its header.

## Design

- Change successful explicit MyISAM scratch tables to Aria where the engine is
  not the tested behavior.
- Keep the embedded-profile `SHOW STATUS` absence explicit in the expected
  result rather than adding a fragile status-variable dependency.
- Add `main.null_key` to the curated MTR smoke list immediately after
  `main.null`.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected MariaDB baseline coverage
for NULL-key optimizer and DML behavior. This is not routed-storage evidence,
does not imply native MyISAM support, and does not change the existing
embedded-profile status metadata limitation.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The test runs in the MTR smoke vardir and
uses the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. MyLite storage-engine NULL-key behavior still belongs in
first-party routed-storage tests or a dedicated routed MTR slice.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.null_key`
- `tools/mylite-mtr-harness run main.null_key`
- `tools/mylite-mtr-harness list`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.null_key` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.null_key`.
- Upstream test-source changes are limited to profile-specific engine and
  status-output normalization.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.null_key`: passed.
- `tools/mylite-mtr-harness run main.null_key`: passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 188
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `196`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test remains baseline-only. Future storage-routed NULL-key coverage should
use MyLite-owned storage tests or a dedicated routed MTR case so it can assert
MyLite handler behavior directly.
