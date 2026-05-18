# Application Foreign-Key Policy

## Problem Statement

The Laravel, Django, and Rails application-schema fixtures originally omitted
foreign-key constraints because MyLite rejected FK DDL before metadata storage
and enforcement existed. The public FK DDL publication and generated-child-key
slices now cover a narrow `RESTRICT` / `NO ACTION` subset, so representative
source-shaped application constraints should be accepted only when they match
that supported subset.

This keeps application-schema coverage honest: MyLite can import the supported
table, row, index, and FK shapes, while broader FK actions and import semantics
remain planned rather than silently ignored.

## Source Findings

- The existing [Foreign-Key Policy](../foreign-key-policy/specs.md) records the
  MariaDB 11.8 parser, SQL-layer, handler, and documentation evidence that
  drove the original rejection policy before MyLite had catalog metadata and
  enforcement.
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
application-shaped acceptance cases:

- Laravel-style `sessions.user_id` constraint referencing `users(id)`.
- Django-style `django_admin_log.user_id` constraint referencing
  `auth_user(id)`.
- Rails Active Storage-style `active_storage_attachments.blob_id` constraint
  referencing `active_storage_blobs(id)`.

Each case creates the supported parent table first, creates the constrained
child table, and verifies catalog publication. The broader FK tests cover
enforcement and close/reopen metadata.

## Compatibility Impact

Application-schema compatibility remains partial. This coverage adds evidence
that common ORM FK shapes in the supported subset are accepted with enforcement
instead of being ignored. Full InnoDB-compatible FK metadata lifecycle,
cascading actions, and import semantics remain planned.

## Single-File And Embedded-Lifecycle Implications

Accepted application-shaped FK DDL must keep all metadata in the primary
`.mylite` file and must not create durable MariaDB sidecars. The existing
storage-smoke close/reopen and sidecar gates remain in force.

## Public API Or File-Format Impact

There is no public API or file-format change. This extends test and
documentation coverage for the public FK subset.

## Storage-Engine Routing Impact

The parent tables route through `ENGINE=InnoDB` to MyLite. The constrained
child tables publish through the MyLite catalog only after handler validation
succeeds.

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

- Laravel-, Django-, and Rails-shaped FK `CREATE TABLE` statements in the
  supported subset publish successfully.
- Accepted application-shaped FK tables are present in the MyLite catalog and
  durable sidecar checks stay clean.
- Compatibility docs and harness text mention application-shaped FK acceptance
  without claiming full FK support.
- No durable MariaDB sidecars are introduced.

## Risks And Unresolved Questions

- Full framework compatibility still needs broader FK metadata lifecycle plus
  adapter or wire-protocol integration.
- Fixture-backed SQL dump import ordering with `foreign_key_checks=0` is covered
  separately; full framework dump/client compatibility remains broader work.
