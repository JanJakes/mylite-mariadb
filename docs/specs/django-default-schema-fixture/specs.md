# Django Default Schema Fixture

## Problem Statement

Application-schema coverage includes WordPress, BuddyPress, Laravel, and a
representative collation matrix. The roadmap still calls out broader ORM suites
and migration runners. MyLite needs an early Django-shaped fixture that
exercises Django's default MySQL/MariaDB table shapes without claiming the
Django migration executor or runtime adapter is supported yet.

Django's default installed applications for admin-oriented projects cover
content types, authentication, admin logs, sessions, many-to-many join tables,
permission rows, migration recorder state, nullable datetimes, `longtext`
payloads, unique constraints, composite indexes, and unsigned small integers.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). The relevant MyLite behavior is
  routed `CREATE TABLE` metadata, row storage, indexes, close/reopen discovery,
  and sidecar lifecycle under the existing MariaDB 11.8 embedded storage-smoke
  build.
- Django source is tag `6.0.5`
  (`c78e404c8981e53786ff9643d1094e0c3265403d`) from `django/django`.
- Django's license is BSD-3-Clause-compatible test fixture source material.
- `django/conf/global_settings.py:439-440` sets the default primary key field
  type to `django.db.models.BigAutoField`.
- `django/contrib/auth/apps.py:13-15`,
  `django/contrib/contenttypes/apps.py:13-15`, and
  `django/contrib/admin/apps.py:7-12` set the built-in auth, contenttypes, and
  admin app default primary key type to `AutoField`.
- `django/contrib/auth/migrations/0001_initial.py:13-203` creates
  `auth_permission`, `auth_group`, `auth_user`, and their many-to-many
  permission/group fields. Later auth migrations alter database-relevant column
  lengths and nullability:
  - `0002_alter_permission_name_max_length.py:10-14` changes permission names to
    255 characters.
  - `0003_alter_user_email_max_length.py:10-15` changes user email to 254
    characters.
  - `0005_alter_user_last_login_null.py:10-15` makes `last_login` nullable.
  - `0008_alter_user_username_max_length.py:10-24` changes username to 150
    characters.
  - `0009_alter_user_last_name_max_length.py:10-15` changes last name to 150
    characters.
  - `0010_alter_group_name_max_length.py:10-13` changes group name to 150
    characters.
  - `0012_alter_user_first_name_max_length.py:10-15` changes first name to 150
    characters.
- `django/contrib/contenttypes/migrations/0001_initial.py:9-44` creates
  `django_content_type`, and
  `0002_remove_content_type_name.py:28-40` removes the legacy `name` column.
- `django/contrib/admin/migrations/0001_initial.py:13-73` creates
  `django_admin_log`; the later admin migrations only change field defaults or
  choices and state that they have no database effect.
- `django/contrib/sessions/migrations/0001_initial.py:9-35` creates
  `django_session` with `session_key`, `session_data`, and indexed
  `expire_date`.
- `django/db/migrations/recorder.py:32-40` defines the
  `django_migrations` recorder table with `app`, `name`, and `applied`.
- `django/db/backends/mysql/base.py:111-138` maps the involved fields to
  MariaDB/MySQL column types: `AutoField` to `integer AUTO_INCREMENT`,
  `BigAutoField` to `bigint AUTO_INCREMENT`, `BooleanField` to `bool`,
  `DateTimeField` to `datetime(6)`, `CharField` to `varchar(...)`,
  `PositiveSmallIntegerField` to `smallint UNSIGNED`, and `TextField` to
  `longtext`.

## Proposed Design

Add `packages/libmylite/tests/fixtures/django-6.0.5-default-schema.sql` with
deterministic MySQL/MariaDB DDL equivalent to Django 6.0.5's default
admin/auth/contenttypes/sessions schema after the listed migrations:

- `django_migrations` uses `bigint` autoincrement for its implicit recorder
  primary key.
- Built-in app tables use `integer` autoincrement IDs where Django pins
  `AutoField`.
- `django_content_type` omits the removed legacy `name` column and keeps the
  unique `(app_label, model)` shape.
- Auth tables include `auth_permission`, `auth_group`, `auth_user`,
  `auth_group_permissions`, `auth_user_groups`, and
  `auth_user_user_permissions`.
- Admin and session tables include `django_admin_log` and `django_session`.
- Foreign-key columns and supporting indexes are preserved, but foreign-key
  constraints are omitted until MyLite implements FK metadata and enforcement.
- Tables use
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci` and omitted-engine
  routing.

Add `packages/libmylite/tests/fixtures/django-6.0.5-default-seed.sql` with
deterministic rows for:

- migration recorder entries for contenttypes, auth, admin, and sessions;
- content type rows for the built-in models;
- default add/change/delete/view permission rows;
- one group, one staff/superuser, many-to-many joins, one session row, and one
  admin log row.

Extend the storage-engine smoke test with a Django fixture case that:

- creates a `django_install` schema with utf8mb4 defaults,
- imports the schema fixture,
- verifies all fixture tables are cataloged with requested engine `DEFAULT`
  and effective engine `MYLITE`,
- imports representative rows and checks forced-index reads across migration,
  content type, permission, group, user, many-to-many, session, and admin-log
  paths,
- verifies table collation metadata for representative tables,
- verifies close/reopen persistence and no durable MariaDB sidecars.

## Affected MariaDB Subsystems

- SQL parser and `CREATE TABLE` execution for Django-style DDL.
- Handler routing for omitted engine requests under the static MyLite storage
  engine.
- Table-definition catalog storage and discovery after close/reopen.
- Row, autoincrement, string primary-key, unique-key, secondary-index, and
  composite-index storage paths.

No upstream MariaDB source changes are expected.

## MySQL/MariaDB Compatibility Impact

The fixture proves representative Django default table definitions can be
accepted by MyLite's MariaDB-derived SQL layer and routed to MyLite storage. It
does not claim Django runtime compatibility, Django's migration executor,
database backend integration, foreign-key enforcement, or broad ORM behavior.

## DDL Metadata Routing Impact

The slice exercises omitted-engine DDL routed to MyLite. Catalog metadata must
preserve table definitions, keys, character set, collation, requested engine
`DEFAULT`, and effective engine `MYLITE` without durable `.frm` files or
engine-specific sidecars.

## Single-File And Embedded-Lifecycle Implications

All durable table metadata, row data, index entries, and autoincrement state
must remain inside the primary `.mylite` file. The test keeps existing
storage-smoke lifecycle gates for close/reopen and forbidden durable sidecars.

## Public API Or File-Format Impact

There is no public API change and no intentional file-format change. The slice
uses existing DDL, row, index, and catalog records.

## Storage-Engine Routing Impact

The fixture uses omitted-engine `CREATE TABLE` statements. They should record
requested engine `DEFAULT` and route to effective engine `MYLITE`.

## Wire-Protocol Or Integration-Package Impact

None. This is embedded storage-smoke coverage only.

## Binary-Size Impact

None expected. The slice adds tests, docs, and SQL fixtures only.

## License Or Dependency Impact

No dependency is added. Django 6.0.5 is BSD-3-Clause licensed.

## Test And Verification Plan

- `cmake --build --preset storage-smoke-dev --target format`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure -R libmylite.embedded-storage-engine`
- `tools/mylite-compat-harness report application-schema`
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

- The Django fixture imports under `storage-smoke-dev`.
- All fixture table metadata is catalog-backed and survives close/reopen.
- Representative keyed row reads pass before and after close/reopen.
- The application-schema harness reports the new coverage.
- Documentation describes Django schema coverage without claiming framework
  runtime or migration-runner support.
- No durable MariaDB sidecars are introduced.

## Risks And Unresolved Questions

- The fixture uses deterministic MySQL/MariaDB DDL equivalent to Django
  migrations; it does not execute Django's migration executor or MySQL backend.
- MyLite still rejects foreign-key constraints, so the fixture preserves
  foreign-key columns and supporting indexes without FK constraints.
- Full Django compatibility still needs adapter or wire-protocol integration,
  migration-runner execution, transactions, generated SQL capture, and
  application-level ORM suites.
