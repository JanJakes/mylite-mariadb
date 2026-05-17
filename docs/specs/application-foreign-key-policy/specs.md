# Application Foreign-Key Policy

## Problem Statement

The Laravel, Django, and Rails application-schema fixtures intentionally omit
foreign-key constraints because MyLite rejects FK DDL until metadata storage
and enforcement exist. The generic foreign-key policy tests already prove that
`FOREIGN KEY` and column-level `REFERENCES` forms are rejected, but the new ORM
fixtures should also prove that source-shaped application constraints fail
explicitly before catalog publication.

This keeps application-schema coverage honest: MyLite can import the supported
table, row, and index shapes, while FK constraints remain planned rather than
silently ignored.

## Source Findings

- The existing [Foreign-Key Policy](../foreign-key-policy/specs.md) records the
  MariaDB 11.8 parser, SQL-layer, handler, and documentation evidence for
  rejecting FK DDL until MyLite has catalog metadata and enforcement.
- Laravel skeleton v13.6.0 default migrations define a nullable indexed user
  reference in `database/migrations/0001_01_01_000000_create_users_table.php`.
  The current fixture preserves the `user_id` column and index but omits a
  constraint.
- Django 6.0.5 auth/admin migrations define `ForeignKey` fields for
  permissions, admin log entries, and many-to-many join tables. The current
  fixture preserves FK columns and supporting indexes but omits constraints.
- Rails v8.1.3 Active Storage migrations declare foreign keys from attachments
  and variant records to blobs. The current fixture preserves `blob_id` columns
  and supporting indexes but omits constraints.

## Proposed Design

Extend `test_transaction_and_foreign_key_policies()` with three
application-shaped rejection cases:

- Laravel-style `sessions.user_id` constraint referencing `users(id)`.
- Django-style `django_admin_log.user_id` constraint referencing
  `auth_user(id)`.
- Rails Active Storage-style `active_storage_attachments.blob_id` constraint
  referencing `active_storage_blobs(id)`.

Each case creates the supported parent table first, attempts the constrained
child table, verifies `assert_foreign_key_exec_fails()`, and checks the failed
child table was not published to the MyLite catalog.

## Compatibility Impact

Application-schema compatibility remains partial. This slice adds evidence that
common ORM FK shapes fail with the stable MyLite FK-policy diagnostic instead
of being accepted without enforcement. Real InnoDB-compatible FK metadata,
enforcement, cascading actions, and import semantics remain planned.

## Single-File And Embedded-Lifecycle Implications

Rejected application-shaped FK DDL must leave the primary `.mylite` catalog
unchanged and must not create durable MariaDB sidecars. The existing
storage-smoke close/reopen and sidecar gates remain in force.

## Public API Or File-Format Impact

There is no public API or file-format change. This extends test and
documentation coverage for an existing policy.

## Storage-Engine Routing Impact

The parent tables route through `ENGINE=InnoDB` to MyLite. The constrained
child-table attempts must reject before MyLite catalog publication.

## Wire-Protocol Or Integration-Package Impact

None. This is embedded storage-smoke coverage only.

## Binary-Size Impact

None expected. The slice adds tests and docs only.

## License Or Dependency Impact

No dependency is added. The source-shaped SQL is derived from already pinned
MIT/BSD-licensed framework schema evidence in the existing application fixture
specs.

## Test And Verification Plan

- `cmake --build --preset storage-smoke-dev --target format`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure -R libmylite.embedded-storage-engine`
- `tools/mylite-compat-harness report foreign-key application-schema`
- `cmake --build --preset dev`
- `ctest --preset dev --output-on-failure`
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- Laravel-, Django-, and Rails-shaped FK `CREATE TABLE` statements reject with
  the stable MyLite FK diagnostic.
- Failed application-shaped FK tables are not present in the MyLite catalog.
- Compatibility docs and harness text mention application-shaped FK rejection
  without claiming FK support.
- No durable MariaDB sidecars are introduced.

## Risks And Unresolved Questions

- This remains rejection coverage. Full framework compatibility still needs real
  FK metadata/enforcement plus adapter or wire-protocol integration.
- SQL dump import flows that disable `foreign_key_checks` still need a dedicated
  policy once MyLite has an FK implementation plan.
