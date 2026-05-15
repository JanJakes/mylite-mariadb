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

The smoke uses a WordPress-shaped subset rather than a full WordPress runtime
install. Later BLOB/TEXT prefix-index, WordPress core schema expansion,
embedded-restart charset, WordPress installer schema fixture, and WordPress
installer seed fixture slices
broadened application-schema coverage with wider `varchar` and
`text`/`longtext` columns, users, usermeta, terms, taxonomy relationships,
comments, commentmeta, links, representative `utf8mb4_unicode_ci` defaults, a
pinned single-site installer DDL fixture, representative installer seed data,
and a small collation restart/index matrix. Exhaustive charset/collation
semantics, multisite, plugin tables, and full runtime install still need broader
support than this smoke should hide inside one test.

## Design

Extend the opt-in `storage-smoke-dev` embedded storage-engine test with a new
application-schema case:

- create `wp_options`, `wp_posts`, and `wp_postmeta` with `ENGINE=InnoDB` and
  `utf8mb4_unicode_ci` defaults;
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

- WordPress-shaped core table DDL for `wp_options`, `wp_posts`,
  `wp_postmeta`, `wp_users`, `wp_usermeta`, `wp_terms`,
  `wp_term_taxonomy`, `wp_term_relationships`, `wp_comments`,
  `wp_commentmeta`, and `wp_links`.
- Routed `ENGINE=InnoDB` catalog metadata and effective `MYLITE` engine.
- Autoincrement `bigint unsigned` ids.
- WordPress-shaped `text`/`longtext` DDL, with `longtext`, `varchar`,
  `datetime`, and integer row values checked by queries.
- Secondary index lookup and a simple join over persisted rows.
- Close/reopen durability and sidecar gates.
- Representative `utf8mb4_unicode_ci` database/table defaults and
  information-schema table-collation checks across reopen.

## Non-Goals

- Full WordPress runtime installer compatibility beyond the versioned DDL and
  seed fixtures.
- Exhaustive character set, collation, and index-length edge cases beyond the
  representative smoke paths.
- Foreign keys or referential enforcement; WordPress itself does not depend on
  database-enforced foreign keys for these core relationships.
- Multisite, plugin tables, migrations, or WP-CLI.

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
- Representative WordPress-shaped tables retain `utf8mb4_unicode_ci` collation
  metadata across close/reopen.
- Queries prove option, post, postmeta, update, delete, secondary-index, and join
  behavior.
- The compatibility harness exposes the new `application-schema` group.
- Roadmap and compatibility docs describe partial application-schema coverage
  without claiming full WordPress support.

## Risks

- The current smoke still separates row/query behavior from the versioned
  WordPress installer fixtures. Full WordPress runtime install coverage would
  make the result look more complete than it is.
- The test still lives in one broad storage-engine smoke binary. Future
  application suites should move into narrower fixtures once the harness grows
  per-application test executables.
