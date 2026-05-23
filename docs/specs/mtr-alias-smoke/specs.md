# MTR Alias Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.alias`. This adds
curated embedded baseline coverage for wildcard-alias parse rejection, valid
wildcard projection aliases, selected CTAS and `SHOW CREATE TABLE` behavior
for unusual generated column names, stored-program execution around generated
aliases, and column-alias metadata.

## Non-Goals

- Broad parser MTR promotion.
- Running the test through MyLite routed storage.
- Enabling native MyISAM in the embedded profile.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/alias.test` covers historical alias parser and
  metadata regressions including invalid `table.* AS alias` forms, valid
  wildcard projections, long malformed `DELETE` parser diagnostics,
  stored-function and stored-procedure alias/name handling, CTAS with unusual
  generated names, and selected metadata output for table aliases.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  scratch MTR tables with Aria. The initial probe failed at the test's one
  explicit `ENGINE=MyISAM` scratch table.
- After replacing that scratch table with Aria, the remaining expected-output
  differences were limited to Aria's `SHOW CREATE TABLE` engine and
  `PAGE_CHECKSUM=1` display for CTAS scratch tables.

## Design

- Change the successful explicit MyISAM scratch table to Aria because native
  MyISAM availability is not the behavior under test.
- Use local `--replace_result` filters around affected `SHOW CREATE TABLE` and
  stored-procedure calls so Aria display details do not fork the upstream
  expected result blocks.
- Add `main.alias` to the curated MTR smoke list with the other parser and
  expression baseline tests.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected MariaDB baseline coverage
for alias parsing, wildcard projection behavior, CTAS generated-name handling,
stored-program alias execution, and column-alias metadata. This is not
routed-storage evidence and does not imply native MyISAM support.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The test runs in the MTR smoke vardir and
uses the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. MyLite storage-engine alias behavior still belongs in
first-party routed-storage tests or a dedicated routed MTR case when it affects
handler behavior.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.alias`
- `tools/mylite-mtr-harness run main.alias`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.alias` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.alias`.
- Upstream test-source changes are limited to profile-specific engine and
  expected-output normalization.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.alias`: passed.
- `tools/mylite-mtr-harness run main.alias`: passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 189
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `197`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test remains baseline-only. Future storage-routed alias coverage should use
MyLite-owned storage tests or a dedicated routed MTR case if alias semantics
interact with handler metadata, table discovery, or routed DDL/DML behavior.
