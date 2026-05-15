# ALTER CHECK Constraint Coverage

## Problem

MyLite already covers basic CHECK constraints created with routed tables:
MariaDB stores the constraint metadata in the table-definition image, evaluates
the expression before MyLite handler writes, and the definition survives
close/reopen through the `.mylite` catalog. The compatibility matrix still
lists CHECK `ALTER` behavior as planned.

This slice adds coverage for `ALTER TABLE ... ADD CONSTRAINT ... CHECK (...)`
and `ALTER TABLE ... DROP CONSTRAINT ...` on supported routed MyLite tables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` sets `ALTER_ADD_CHECK_CONSTRAINT` for
  `ALTER TABLE ... ADD CHECK` / `ADD CONSTRAINT ... CHECK` and
  `ALTER_DROP_CHECK_CONSTRAINT` for `DROP CONSTRAINT`.
- `mariadb/sql/handler.h` defines the CHECK ALTER operation flags used by the
  SQL layer and handler alter planning.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes duplicate
  `ADD CONSTRAINT IF NOT EXISTS` checks and keeps the alter flags aligned with
  the remaining constraint list.
- `mariadb/sql/sql_table.cc:fix_constraints_names()` generates missing CHECK
  names during CREATE and ALTER.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` builds the new
  constraint list by preserving existing constraints, applying requested drops,
  and appending requested additions before the replacement definition is
  created.
- `mariadb/sql/handler.cc:handler::check_if_supported_inplace_alter()` treats
  `ALTER_DROP_CHECK_CONSTRAINT` as an offline in-place candidate for engines
  that implement that path. MyLite currently keeps the default
  `check_if_incompatible_data()` copy-rebuild behavior, which is the safer
  path while catalog publication is append-only.
- The existing CHECK slice records that `mariadb/sql/table.cc` reopens CHECK
  metadata from the table-definition image and that insert/update paths call
  `TABLE::verify_constraints()` before handler writes.

## Design

Do not add MyLite parser or expression-evaluation code. Continue to let
MariaDB own CHECK parsing, validation, row-copy validation, and runtime
evaluation.

Add storage-engine smoke coverage on an existing routed `ENGINE=InnoDB` table:

- drop a named CHECK constraint through copy ALTER,
- prove writes that would have violated the dropped constraint now succeed,
- add a new named CHECK constraint through copy ALTER,
- prove a CHECK added through ALTER can be dropped in the same opened runtime,
  and
- prove a CHECK added through ALTER is still enforced after close/reopen.

The later catalog-reopen copy ALTER slice extends this coverage to dropping an
ALTER-added CHECK after catalog-only close/reopen.

The smoke table already has one row inserted while
`check_constraint_checks=OFF`; using values that satisfy the newly added
constraint keeps the ADD path focused on supported row-copy behavior rather
than failed ALTER rollback.

## Supported Scope

- Named table-level CHECK drops on supported routed MyLite tables.
- Named table-level CHECK additions on supported routed MyLite tables.
- Enforcement of added CHECK constraints across close/reopen.
- Dropping an ALTER-added named CHECK in the same opened runtime.
- Disabled-check rows that remain valid for the new constraint.

## Non-Goals

- Failed ADD CHECK rollback when existing rows violate the new constraint.
- Column-level CHECK drops by generated internal name.
- `ADD CONSTRAINT IF NOT EXISTS`, unnamed CHECK name generation, or duplicate
  CHECK diagnostics.
- Prepared-statement-specific diagnostics.
- MTR-scale CHECK expression coverage.
- MyLite-native CHECK expression storage or evaluation.

## Compatibility Impact

MyLite moves basic CHECK `ALTER` behavior for supported routed tables from
planned to covered. Broader CHECK expressions, failed ALTER rollback,
CTAS/dump-import edge cases, and prepared diagnostics remain planned.

## DDL Metadata Routing Impact

CHECK additions and drops change the MariaDB table-definition image that MyLite
stores in the catalog. No separate MyLite semantic constraint catalog is added
in this slice.

## Single-File And Embedded-Lifecycle Impact

No new companion files or sidecars are introduced. CHECK metadata stays in the
primary `.mylite` catalog-backed table-definition record and is rediscovered
after close/reopen.

## Public API And File-Format Impact

The public `libmylite` API and storage file format do not change.

## Storage-Engine Routing Impact

The behavior applies to all supported routed engine requests through the shared
MyLite handler and catalog path. This slice tests `ENGINE=InnoDB` routing
because it is the main compatibility target for application schemas.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol or integration-package changes are included.

## Binary-Size And Dependency Impact

No dependency is added. The implementation is expected to be test and
documentation coverage over existing MariaDB SQL behavior.

## Test And Verification Plan

- Extend storage-engine smoke coverage with CHECK add/drop ALTER statements.
- Verify same-runtime drop behavior and close/reopen enforcement.
- Update the CHECK constraint compatibility docs and roadmap.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, and
  the CHECK/routed DDL compatibility harness groups.

## Acceptance Criteria

- Dropping a named CHECK constraint lets a formerly invalid write succeed.
- Adding a named CHECK constraint rejects invalid writes.
- Added CHECK metadata is enforced after close/reopen.
- Dropping an ALTER-added CHECK removes that enforcement in the same opened
  runtime.
- Docs and compatibility tables no longer list basic CHECK ALTER as
  planned-only.

## Risks And Unresolved Questions

- Failed ADD CHECK over incompatible existing rows remains a separate rollback
  slice because DDL rollback is broader than this coverage-only change.
- Metadata-only CHECK ALTER support must be reconsidered when MyLite adds an
  in-place ALTER path; this slice intentionally stays on copy rebuilds.
