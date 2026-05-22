# MTR Trigger DDL Runtime Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.create_drop_trigger`.
This adds upstream embedded baseline coverage for selected trigger DDL,
trigger execution, trigger metadata, `IF NOT EXISTS`, `OR REPLACE`, duplicate
diagnostics, and drop behavior under the MTR profile.

## Non-Goals

- Claiming default-product trigger runtime support.
- Implementing catalog-backed MyLite triggers.
- Running MTR against MyLite storage-engine routing.
- Changing parser, trigger, stored-program, metadata, storage behavior, or file
  format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/create_drop_trigger.test` creates a schema and table,
  creates `BEFORE INSERT` and `AFTER INSERT` triggers, verifies row-triggered
  user-variable effects, checks `INFORMATION_SCHEMA.TRIGGERS`, and exercises
  duplicate/replacement/drop diagnostics.
- `mariadb/sql/sql_yacc.yy` parses trigger DDL into `SQLCOM_CREATE_TRIGGER`
  and `SQLCOM_DROP_TRIGGER`.
- `mariadb/sql/sql_parse.cc` dispatches trigger DDL through
  `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_trigger.cc` owns file-backed trigger metadata, trigger
  creation/drop, trigger loading through `Table_triggers_list::check_n_load()`,
  and execution through `Table_triggers_list::process_triggers()`.
- `mariadb/sql/sql_base.cc` invokes trigger loading while opening table shares
  and trigger execution before/after row operations.
- `mariadb/sql/sql_show.cc` uses trigger loading helpers for trigger metadata.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.create_drop_trigger` passed without
  upstream test edits.

## Design

- Add `main.create_drop_trigger` near the view and DDL/name MTR smoke tests in
  the curated harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to MTR-profile trigger DDL/runtime coverage. The default MyLite
  product profile still rejects persistent triggers before MariaDB can publish
  filesystem metadata or execute trigger bodies.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected trigger DDL/runtime behavior under the MTR profile. This is MariaDB
embedded compatibility evidence, not a new MyLite trigger-runtime or
catalog-backed trigger support claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir and creates only transient MTR schema, table, and trigger metadata.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.create_drop_trigger`
- `tools/mylite-mtr-harness run main.create_drop_trigger`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.create_drop_trigger` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to MTR-profile trigger DDL/runtime behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.create_drop_trigger`: passed.
- `tools/mylite-mtr-harness run main.create_drop_trigger`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 175 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `176`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This test intentionally exercises MariaDB's file-backed trigger runtime inside
the opt-in MTR profile. Future catalog-backed MyLite trigger support still
needs first-party product tests for DDL, metadata, statement integration,
trigger body execution, transaction/rollback ordering, and single-file
lifecycle.
