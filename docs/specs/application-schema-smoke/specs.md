# Application Schema Smoke

## Problem

MyLite has storage-engine smoke coverage for small synthetic tables, but real
applications combine larger DDL, MySQL/MariaDB defaults, secondary indexes,
text payloads, autoincrement ids, and common query shapes. The next compatibility
step is to prove a representative application-shaped schema can be created,
used, closed, and reopened without durable MariaDB sidecars.

This slice adds the first application-schema smoke: a WordPress-shaped subset of
core tables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc` and the MyLite handler path already route supported
  `CREATE TABLE ... ENGINE=InnoDB` definitions to the MyLite handler.
- `mariadb/sql/handler.cc` drives table scans, key reads, updates, deletes, and
  table-copy operations through handler calls that the existing smoke covers for
  smaller tables.
- `mariadb/storage/mylite/ha_mylite.cc` reconstructs MariaDB `Field_blob`
  records from MyLite row payloads. Joined result rows can outlive scan or index
  cursor buffers, so returned `text` and `longtext` fields need per-handler
  current-row payload ownership.
- The WordPress Codex database description identifies `wp_options`,
  `wp_postmeta`, and `wp_posts` as core tables, with `bigint unsigned`
  autoincrement ids, `longtext` content/meta/option values, `varchar` keys,
  secondary indexes, and MySQL/MariaDB as the supported database family:
  <https://codex.wordpress.org/Database_Description>.

The smoke uses a WordPress-shaped subset rather than an exact current WordPress
installer dump. Exact WordPress schema import, charset/collation variants, and
plugin tables need broader support than this first slice should hide inside one
test. A later BLOB/TEXT prefix-index slice broadened this smoke with prefix
indexes on wider `varchar` and `text`/`longtext` columns.

## Design

Extend the opt-in `storage-smoke-dev` embedded storage-engine test with a new
application-schema case:

- create `wp_options`, `wp_posts`, and `wp_postmeta` with `ENGINE=InnoDB`;
- cover supported prefix key shapes, including `meta_key(191)` on
  `varchar(255)` and a bounded `text` prefix index;
- insert option, post, and postmeta rows with `bigint unsigned`, `datetime`,
  `varchar`, `text`, and `longtext` values;
- query by unique and secondary indexes;
- run a join across posts and postmeta, including `text` and `longtext`
  selected from both sides of the join;
- update and delete rows;
- close and reopen the `.mylite` file and re-check the routed table metadata and
  values.

Add a compatibility harness group named `application-schema` that runs the
storage-engine smoke label carrying this test.

## Supported Scope

- WordPress-shaped core table DDL for `wp_options`, `wp_posts`, and
  `wp_postmeta`.
- Routed `ENGINE=InnoDB` catalog metadata and effective `MYLITE` engine.
- Autoincrement `bigint unsigned` ids.
- WordPress-shaped `text`/`longtext` DDL, with `longtext`, `varchar`,
  `datetime`, and integer row values checked by queries.
- Secondary index lookup and a simple join over persisted rows.
- Close/reopen durability and sidecar gates.

## Non-Goals

- Full WordPress installer compatibility.
- Character set, collation, and index-length edge cases.
- Foreign keys or referential enforcement; WordPress itself does not depend on
  database-enforced foreign keys for these core relationships.
- Multisite, users, comments, terms, plugin tables, migrations, or WP-CLI.

## Compatibility Impact

Application-schema coverage moves from planned to partial. The compatibility
matrix should say that a WordPress-shaped core-table subset is covered by
storage-smoke tests, while full WordPress remains planned.

## File-Lifecycle Impact

The smoke writes only the primary `.mylite` file plus existing MyLite-owned
temporary runtime files. It reuses the storage-engine sidecar scanner to reject
durable `.frm`, InnoDB, MyISAM, Aria, binlog, and relay-log companions.

## Test Plan

- Add the WordPress-shaped storage-engine smoke case.
- Add `compat-application-schema` labels and harness group wiring.
- Run `tools/mylite-compat-harness run application-schema`.
- Run dev, embedded, storage-smoke, tidy, format, diff, and archive-size checks.

## Acceptance Criteria

- The WordPress-shaped smoke creates and reopens all routed tables without
  durable sidecars.
- Queries prove option, post, postmeta, update, delete, secondary-index, and join
  behavior.
- The compatibility harness exposes the new `application-schema` group.
- Roadmap and compatibility docs describe partial application-schema coverage
  without claiming full WordPress support.

## Risks

- The current smoke uses a subset of WordPress's schema because broader
  collation behavior and the full installer schema would otherwise make the
  result look more complete than it is.
- The test still lives in one broad storage-engine smoke binary. Future
  application suites should move into narrower fixtures once the harness grows
  per-application test executables.
