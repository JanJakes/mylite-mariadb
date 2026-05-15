# WordPress Core Schema Expansion

## Problem

The first application-schema smoke covered a reduced WordPress-shaped
`wp_options`, `wp_posts`, and `wp_postmeta` subset. That was useful, but too
small to exercise the wider MySQL/MariaDB DDL shapes common in WordPress core:
user metadata, term taxonomy relationships, comments, comment metadata, links,
composite keys, text families, varchar prefix indexes, and several secondary
lookup patterns.

MyLite needs broader application-shaped evidence before moving from synthetic
storage tests to installer- or ORM-scale suites.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- The existing `application-schema-smoke` slice records the WordPress Codex
  database description as compatibility evidence for WordPress core table
  shapes and MySQL/MariaDB as the target database family.
- `mariadb/sql/sql_table.cc` routes supported `CREATE TABLE ... ENGINE=InnoDB`
  definitions into the MyLite handler when the storage-smoke profile is active.
- `mariadb/storage/mylite/ha_mylite.cc` now covers routed table creation,
  composite primary and secondary keys, bounded prefix indexes over
  `varchar`/text-like fields, joins, update/delete, and close/reopen
  discovery.
- Existing MyLite storage pages can represent the DDL and row values needed for
  a broader WordPress core-table subset without adding file-format features.

## Design

Expand the storage-smoke WordPress-shaped application-schema test from three
tables to the currently supported core-table subset:

- existing `wp_options`, `wp_posts`, and `wp_postmeta`;
- `wp_users` and `wp_usermeta`;
- `wp_terms`, `wp_term_taxonomy`, and `wp_term_relationships`;
- `wp_comments` and `wp_commentmeta`;
- `wp_links`.

Keep the schema WordPress-shaped rather than an exact installer dump. The test
uses the same MySQL/MariaDB column families, default patterns, autoincrement
ids, composite keys, and representative prefix indexes, but still leaves exact
charset/collation variants and full installer import to later suites.

The smoke should insert representative rows, query through secondary indexes,
join taxonomy and postmeta relationships, update and delete rows, close/reopen
the file, and repeat enough checks to prove catalog discovery and persisted
row/index state.

## Supported Scope

- Broader WordPress core-table DDL routed from `ENGINE=InnoDB` to MyLite.
- `BIGINT UNSIGNED` autoincrement ids, `DATETIME`, `VARCHAR`, `TINYTEXT`,
  `TEXT`, `MEDIUMTEXT`, and `LONGTEXT` values.
- Composite primary and unique keys.
- Bounded prefix indexes over `varchar` fields.
- Representative joins across posts/postmeta and terms/taxonomy/relationships.
- Update/delete and close/reopen visibility.
- Durable sidecar gates for the broader schema.

## Non-Goals

- Exact WordPress installer import.
- Charset, collation, and index-length matrix coverage.
- Multisite tables, plugin tables, migrations, or WP-CLI.
- Foreign-key enforcement; WordPress core table relationships are not enforced
  by database foreign keys.
- Transaction rollback beyond existing statement-level coverage.

## Compatibility Impact

Application-schema compatibility remains partial but materially broader. The
coverage matrix should name the broader WordPress-shaped core table set while
still making full WordPress support planned.

## File Lifecycle

No file-format or durable companion changes are introduced. The storage-smoke
sidecar scanner must still find only the primary `.mylite` file and
MyLite-owned temporary runtime state.

## Embedded Lifecycle And API

No public API change. The test still exercises file-owned open/close and
catalog rediscovery through `libmylite`.

## Build, Size, And Dependencies

No dependency or default build-profile change. The added coverage extends an
existing storage-smoke executable.

## Test Plan

- Extend `test_wordpress_shaped_schema()` with the broader core-table subset.
- Run the targeted storage-engine smoke test.
- Run the `application-schema` compatibility harness group.
- Run format, tidy, dev, embedded, and storage-smoke verification before
  commit.

## Acceptance Criteria

- The broader WordPress-shaped core schema creates through routed
  `ENGINE=InnoDB` DDL without durable MariaDB sidecars.
- Catalog metadata records all covered tables as requested `InnoDB` and
  effective `MYLITE` before and after reopen.
- Insert, indexed lookup, join, update, delete, and close/reopen checks pass for
  the expanded schema.
- Docs and compatibility status describe broader partial coverage without
  claiming full WordPress compatibility.

## Risks And Open Questions

- A larger single smoke test gives useful compatibility pressure, but it is not
  a substitute for a real WordPress installer/import suite.
- Exact WordPress schema versions and charset defaults will need a versioned
  fixture strategy before MyLite can claim installer compatibility.
