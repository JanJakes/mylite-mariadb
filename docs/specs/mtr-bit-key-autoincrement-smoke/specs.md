# MTR BIT-Key Autoincrement Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.mdev_14586`. This adds
upstream embedded baseline coverage for `AUTO_INCREMENT` allocation when the
autoincrement column is paired with `BIT` columns in secondary or composite
keys, including grouped allocation by key prefix and dropping a primary key
after adding a secondary key.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Changing MyLite autoincrement allocation, index metadata, storage behavior,
  or file format.
- Claiming exhaustive grouped autoincrement compatibility.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/mdev_14586.test` creates tables that combine
  `AUTO_INCREMENT` columns with `BIT(1)` and `BIT(5)` key parts, verifies
  generated values per key prefix, and checks an `ALTER TABLE ... DROP PRIMARY
  KEY` path after adding an index over `(b, pk)`.
- `mariadb/sql/handler.h` and handler implementations use
  `next_number_keypart` / `next_number_index` metadata to identify grouped
  autoincrement allocation over key prefixes.
- MyLite has separate first-key and grouped autoincrement code paths for routed
  storage, but this MTR slice validates MariaDB embedded baseline behavior
  under the smoke profile rather than routed MyLite tables.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.mdev_14586` passed without upstream
  test edits.

## Design

- Add `main.mdev_14586` next to the existing autoincrement-oriented MTR smoke
  tests in the curated harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to selected BIT-key autoincrement baseline behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for a
subtle autoincrement/key-prefix regression. This is MariaDB embedded
compatibility evidence, not a new routed-storage behavior claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir with Aria files owned by the MTR run.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.mdev_14586`
- `tools/mylite-mtr-harness run main.mdev_14586`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.mdev_14586` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected BIT-key autoincrement behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.mdev_14586`: passed.
- `tools/mylite-mtr-harness run main.mdev_14586`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 172 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `173`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test uses Aria scratch tables in MTR, not MyLite-routed storage. Future
MyLite grouped autoincrement compatibility claims need first-party storage-smoke
coverage over MyLite handler tables.
