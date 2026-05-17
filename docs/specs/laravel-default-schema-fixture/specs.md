# Laravel Default Schema Fixture

## Problem Statement

Application-schema coverage now includes WordPress, WordPress multisite, and a
large GPLv2-compatible plugin schema. The roadmap still calls out ORM suites.
MyLite needs an early ORM-shaped fixture that exercises framework-generated
MySQL table shapes without implying that the framework migration runner is
supported yet.

Laravel's application skeleton ships with default migrations for users,
sessions, cache, queued jobs, job batches, and failed jobs. These tables cover
common ORM and queue schema shapes: string primary keys, unique indexes,
nullable timestamps, text payloads, unsigned counters, composite indexes, and
autoincrement ids.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). The relevant MyLite behavior is
  routed `CREATE TABLE` metadata, row storage, indexes, close/reopen discovery,
  and sidecar lifecycle under the existing MariaDB 11.8 embedded storage-smoke
  build.
- Laravel application skeleton source is tag `v13.6.0`
  (`a29735c8a1d2e9026da8b25a74e662864829f506`) from `laravel/laravel`.
- `composer.json:3-10` identifies the skeleton as `laravel/laravel`, declares
  MIT licensing, and depends on `laravel/framework`.
- `database/migrations/0001_01_01_000000_create_users_table.php:14-37`
  defines `users`, `password_reset_tokens`, and `sessions`.
- `database/migrations/0001_01_01_000001_create_cache_table.php:14-24`
  defines `cache` and `cache_locks`.
- `database/migrations/0001_01_01_000002_create_jobs_table.php:14-47`
  defines `jobs`, `job_batches`, and `failed_jobs`, including a composite index
  over connection, queue, and failed timestamp.

## Proposed Design

Add `packages/libmylite/tests/fixtures/laravel-13.6.0-default-schema.sql`
with deterministic MySQL/MariaDB DDL equivalent to the Laravel skeleton
migrations:

- `id()` becomes `bigint unsigned not null auto_increment primary key`.
- `string()` becomes `varchar(255)` unless the migration specifies a shorter
  length.
- `rememberToken()` becomes `remember_token varchar(100) null`.
- `timestamps()` becomes nullable `created_at` and `updated_at` timestamp
  columns.
- `timestamp()->nullable()` becomes a nullable timestamp column.
- `timestamp()->useCurrent()` becomes `timestamp not null default
  current_timestamp`.
- `foreignId()->nullable()->index()` is represented as a nullable unsigned
  bigint with a secondary index, but without a foreign-key constraint. This
  matches MyLite's current foreign-key policy while preserving the stored
  column/index shape.
- Laravel table and index names use the framework's conventional names.
- Tables use
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci` and omitted-engine
  routing.

Add `packages/libmylite/tests/fixtures/laravel-13.6.0-default-seed.sql` with
deterministic rows for every table.

Extend the storage-engine smoke test with a Laravel fixture case that:

- creates a `laravel_install` schema with WordPress-style utf8mb4 defaults,
- imports the schema fixture,
- verifies all fixture tables are cataloged with requested engine `DEFAULT`
  and effective engine `MYLITE`,
- imports representative rows and checks forced-index reads across user,
  session, cache, cache lock, queued job, job batch, and failed-job paths,
- verifies table collation metadata for representative tables,
- verifies close/reopen persistence and no durable MariaDB sidecars.

## Affected MariaDB Subsystems

- SQL parser and `CREATE TABLE` execution for Laravel-style DDL.
- Handler routing for omitted engine requests under the static MyLite storage
  engine.
- Table-definition catalog storage and discovery after close/reopen.
- Row, autoincrement, string primary-key, unique-key, secondary-index, and
  composite-index storage paths.

No upstream MariaDB source changes are expected.

## MySQL/MariaDB Compatibility Impact

The fixture proves representative Laravel default table definitions can be
accepted by MyLite's MariaDB-derived SQL layer and routed to MyLite storage. It
does not claim Laravel runtime compatibility, `artisan migrate` support,
Illuminate schema grammar support, foreign-key enforcement, or broad Eloquent
behavior.

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

No dependency is added. The Laravel skeleton declares MIT licensing in
`composer.json:3-10`.

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

- The Laravel fixture imports under `storage-smoke-dev`.
- All fixture table metadata is catalog-backed and survives close/reopen.
- Representative keyed row reads pass before and after close/reopen.
- The application-schema harness reports the new coverage.
- Documentation describes Laravel schema coverage without claiming framework
  runtime or migration-runner support.
- No durable MariaDB sidecars are introduced.

## Risks And Unresolved Questions

- The fixture uses deterministic MySQL DDL equivalent to Laravel migrations;
  it does not execute Laravel's migration grammar.
- MyLite still rejects foreign-key constraints, so the sessions `user_id`
  column is represented without a foreign-key constraint.
- Full ORM compatibility still needs migration-runner integration, generated
  SQL capture, connection configuration, transactions, and application-level
  CRUD suites.
