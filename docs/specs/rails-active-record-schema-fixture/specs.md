# Rails Active Record Schema Fixture

## Problem Statement

Application-schema coverage now includes WordPress, BuddyPress, Laravel,
Django, and a representative collation matrix. The roadmap still calls out
broader ORM suites and migration runners. MyLite needs a Rails-shaped fixture
that exercises Active Record's MySQL/MariaDB DDL conventions without claiming
that Rails migrations or the Rails runtime can execute against MyLite yet.

Rails applications commonly carry Active Record's internal migration metadata
tables plus framework component schemas such as Active Storage and Action Text.
Those tables cover string primary keys, bigint autoincrement primary keys,
polymorphic references, text and longtext payloads, datetime precision,
composite unique indexes, and foreign-key columns.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). The relevant MyLite behavior is
  routed `CREATE TABLE` metadata, row storage, indexes, close/reopen discovery,
  and sidecar lifecycle under the existing MariaDB 11.8 embedded storage-smoke
  build.
- Rails source is tag `v8.1.3`
  (`90588c21894456d979d7195502e6f5918f8d59ea`) from `rails/rails`.
- Rails is MIT licensed.
- `activerecord/lib/active_record/schema_migration.rb:53-58` creates
  `schema_migrations` with no `id` column and a string `version` primary key.
- `activerecord/lib/active_record/internal_metadata.rb:84-94` creates
  `ar_internal_metadata` with no `id` column, string primary key `key`, string
  `value`, and timestamps.
- `activerecord/lib/active_record/connection_adapters/abstract_mysql_adapter.rb:31-47`
  maps Active Record MySQL types: `primary_key` to `bigint auto_increment
  PRIMARY KEY`, `string` to `varchar(255)`, `text` to `text`, `bigint` to
  `bigint`, `datetime` to `datetime`, and `boolean` to `boolean`.
- `activerecord/lib/active_record/connection_adapters/abstract_mysql_adapter.rb:144-145`
  reports MySQL datetime precision support.
- `activerecord/lib/active_record/connection_adapters/abstract/schema_definitions.rb:521-533`
  makes `timestamps` add `created_at` and `updated_at` as non-null
  `datetime(6)` columns when the adapter supports precision.
- `activerecord/lib/active_record/connection_adapters/abstract/schema_definitions.rb:202-289`
  defines references, including polymorphic `<name>_type` and `<name>_id`
  columns.
- `activestorage/db/migrate/20170806125915_create_active_storage_tables.rb:1-55`
  creates `active_storage_blobs`, `active_storage_attachments`, and
  `active_storage_variant_records`, including unique indexes and foreign-key
  declarations.
- `actiontext/db/migrate/20180528164100_create_action_text_tables.rb:1-24`
  creates `action_text_rich_texts`, including long text body storage,
  polymorphic references, timestamps, and a composite unique index.

## Proposed Design

Add `packages/libmylite/tests/fixtures/rails-8.1.3-active-record-schema.sql`
with deterministic MySQL/MariaDB DDL equivalent to the Rails 8.1.3 Active
Record metadata, Active Storage, and Action Text migrations:

- `schema_migrations` uses a string primary key and no `id`.
- `ar_internal_metadata` uses a string primary key and `datetime(6)`
  timestamps.
- Active Storage tables use bigint autoincrement primary keys, bigint
  references, `text` metadata, `datetime(6)` timestamps, and the source-named
  unique indexes.
- Action Text uses bigint autoincrement primary key, polymorphic reference
  columns, `longtext` body storage, `datetime(6)` timestamps, and the
  source-named unique index.
- Foreign-key columns and supporting indexes are preserved, but foreign-key
  constraints are omitted until MyLite implements FK metadata and enforcement.
- Tables use
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci` and omitted-engine
  routing.

Add `packages/libmylite/tests/fixtures/rails-8.1.3-active-record-seed.sql` with
deterministic rows for migration metadata, internal metadata, one Active
Storage blob, one attachment, one variant record, and one Action Text rich-text
record.

Extend the storage-engine smoke test with a Rails fixture case that:

- creates a `rails_install` schema with utf8mb4 defaults,
- imports the schema fixture,
- verifies all fixture tables are cataloged with requested engine `DEFAULT`
  and effective engine `MYLITE`,
- imports representative rows and checks primary, unique, secondary, and
  composite indexed reads,
- verifies table collation metadata for representative tables,
- verifies close/reopen persistence and no durable MariaDB sidecars.

## Affected MariaDB Subsystems

- SQL parser and `CREATE TABLE` execution for Active Record-style DDL.
- Handler routing for omitted engine requests under the static MyLite storage
  engine.
- Table-definition catalog storage and discovery after close/reopen.
- Row, autoincrement, string primary-key, unique-key, secondary-index,
  composite-index, `text`, and `longtext` storage paths.

No upstream MariaDB source changes are expected.

## MySQL/MariaDB Compatibility Impact

The fixture proves representative Rails component table definitions can be
accepted by MyLite's MariaDB-derived SQL layer and routed to MyLite storage. It
does not claim Rails runtime compatibility, Rails migration execution,
mysql2/trilogy adapter integration, foreign-key enforcement, or broad Active
Record behavior.

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

No dependency is added. Rails v8.1.3 is MIT licensed.

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

- The Rails Active Record fixture imports under `storage-smoke-dev`.
- All fixture table metadata is catalog-backed and survives close/reopen.
- Representative keyed row reads pass before and after close/reopen.
- The application-schema harness reports the new coverage.
- Documentation describes Rails schema coverage without claiming framework
  runtime or migration-runner support.
- No durable MariaDB sidecars are introduced.

## Risks And Unresolved Questions

- The fixture uses deterministic MySQL/MariaDB DDL equivalent to Rails
  migrations; it does not execute Rails migrations or adapters.
- MyLite still rejects foreign-key constraints, so the fixture preserves
  foreign-key columns and supporting indexes without FK constraints.
- Full Rails compatibility still needs adapter or wire-protocol integration,
  migration-runner execution, transactions, generated SQL capture, and
  application-level Active Record suites.
