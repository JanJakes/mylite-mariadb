# MTR View DDL Runtime Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.create_drop_view`. This
adds upstream embedded baseline coverage for selected view DDL, view metadata,
view row reads, `IF NOT EXISTS`, `OR REPLACE`, and drop-type diagnostics under
the MTR profile.

## Non-Goals

- Claiming default-product view runtime support.
- Implementing catalog-backed MyLite views.
- Running MTR against MyLite storage-engine routing.
- Changing parser, view, metadata, storage behavior, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/create_drop_view.test` creates a base table, creates
  and replaces a view, reads rows through the view, checks
  `INFORMATION_SCHEMA.VIEWS`, and verifies drop diagnostics for table/view type
  mismatches.
- `mariadb/sql/sql_yacc.yy` parses view DDL into `SQLCOM_CREATE_VIEW` and
  `SQLCOM_DROP_VIEW`.
- `mariadb/sql/sql_parse.cc` dispatches view DDL through
  `mysql_create_view()` and `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc` owns file-backed view registration, view loading
  through `mysql_make_view()`, replacement, and drop behavior for the upstream
  embedded profile.
- `mariadb/sql/sql_base.cc` and `mariadb/sql/sql_show.cc` call
  `mysql_make_view()` while opening view shares and populating view metadata.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.create_drop_view` passed without
  upstream test edits.

## Design

- Add `main.create_drop_view` near the DDL/name MTR smoke tests in the curated
  harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to MTR-profile view DDL/runtime coverage. The default MyLite
  product profile still rejects persistent views before MariaDB can publish
  filesystem metadata.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected view DDL/runtime behavior under the MTR profile. This is MariaDB
embedded compatibility evidence, not a new MyLite view-runtime or catalog-backed
view support claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir and creates only transient MTR table/view metadata.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.create_drop_view`
- `tools/mylite-mtr-harness run main.create_drop_view`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.create_drop_view` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to MTR-profile view DDL/runtime behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.create_drop_view`: passed.
- `tools/mylite-mtr-harness run main.create_drop_view`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 174 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `175`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This test intentionally exercises MariaDB's file-backed view runtime inside the
opt-in MTR profile. Future catalog-backed MyLite view support still needs
first-party product tests for DDL, metadata, dependency invalidation, row reads,
updatability policy, and single-file lifecycle.
