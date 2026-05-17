# WordPress Multisite Blog Schema Fixture

## Problem

MyLite now imports WordPress multisite global tables, but a multisite network
also creates blog-specific tables for each site. Those tables use per-blog
prefixes such as `wp_2_posts`, which add application-schema pressure that the
global fixture does not cover.

This slice adds a bounded WordPress multisite per-blog schema fixture and tests
it alongside the multisite global fixture. It proves the combined schema imports
and survives close/reopen without claiming WordPress multisite runtime support.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- WordPress `wp-admin/includes/schema.php:36-53` defines
  `wp_get_db_schema()`, switches `$wpdb` to the requested `$blog_id` when one
  is supplied, and fixes `$max_index_length = 191`.
- `schema.php:55-189` defines the blog-specific tables used for posts,
  comments, links, options, terms, taxonomy, relationships, and metadata.
- `schema.php:318-337` returns only blog-specific tables for the `blog` scope
  and combines global plus blog tables for `all`.
- `wp-includes/class-wpdb.php:1049-1095` updates the active blog id and derives
  the blog prefix. In multisite, non-main site id `2` maps to
  `$base_prefix . '2_'`.
- `class-wpdb.php:1128-1172` maps global tables to the base prefix and blog
  tables to the active blog prefix.
- The DDL has no `ENGINE` clauses. In the MyLite storage-smoke profile,
  omitted engines route to MyLite and are cataloged as requested `DEFAULT`,
  effective `MYLITE`.

Reference URLs:

- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/schema.php>
- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-includes/class-wpdb.php>
- <https://wordpress.org/documentation/article/wordpress-versions/>

## Design

Add `wordpress-6.9.4-multisite-blog-2-schema.sql` beside the existing
WordPress fixtures. The fixture is deterministic SQL derived from
`wp_get_db_schema('blog', 2)`:

- `$wpdb` blog table names use the `wp_2_` prefix;
- `$max_index_length` is substituted with `191`;
- `$charset_collate` is substituted with
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`;
- no `ENGINE` clauses are added.

Extend the storage-smoke application-schema coverage with a combined multisite
database that imports the existing multisite global fixture and the new
blog-specific fixture, checks catalog metadata for all 18 tables, inserts a
representative network/blog row set, exercises indexed reads over `wp_blogs`
and `wp_2_*` tables, closes/reopens the `.mylite` file, and repeats metadata
and row assertions.

## Supported Scope

- WordPress 6.9.4 multisite blog schema DDL for blog id `2`.
- `wp_2_termmeta`, `wp_2_terms`, `wp_2_term_taxonomy`,
  `wp_2_term_relationships`, `wp_2_commentmeta`, `wp_2_comments`,
  `wp_2_links`, `wp_2_options`, `wp_2_postmeta`, and `wp_2_posts`.
- Combined import with the multisite global table fixture.
- Omitted-engine routing to MyLite.
- `utf8mb4_unicode_ci` table defaults.
- Catalog metadata, representative row/index visibility, and close/reopen
  discovery.

## Non-Goals

- Running WordPress PHP, WP-CLI, or multisite network creation flow.
- Seeding exhaustive blog defaults, network options, uploads paths, cron,
  rewrite state, mail, or upgrade paths.
- Multiple per-blog prefixes beyond blog id `2`.
- Plugin tables or broader ORM suites.
- Foreign-key enforcement; WordPress multisite blog tables do not declare
  database foreign keys.

## Compatibility Impact

Application-schema compatibility remains partial, but MyLite gains committed
coverage for a representative multisite per-blog table prefix and combined
multisite catalog import. The compatibility matrix should still distinguish
this from full WordPress multisite runtime support.

## DDL Metadata Routing Impact

The fixture exercises omitted-engine table DDL, so all imported table catalog
records should store requested engine `DEFAULT` and effective engine `MYLITE`.
The test also checks representative table collations through
`INFORMATION_SCHEMA.TABLES`.

## Single-File And Embedded-Lifecycle Impact

The import and row checks must leave only the primary `.mylite` file after
close. Runtime directories remain MyLite-owned transient state and are removed
on final close.

## Public API Or File-Format Impact

No public API or file-format change is intended. The slice uses existing
`mylite_exec()`, catalog, row, index, and close/reopen behavior.

## Storage-Engine Routing Impact

No new engine mapping is added. The fixture adds compatibility pressure to the
existing default-engine route.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

No default MariaDB build-profile or linked-runtime change is expected. This is
test fixture and documentation work.

## License Or Dependency Impact

The fixture is derived from GPL-2.0-or-later WordPress schema DDL and is
compatible with MyLite's GPL-2.0 MariaDB-derived repository. No dependency is
added.

## Test And Verification Plan

- Add the versioned SQL fixture.
- Add storage-smoke coverage that imports global and blog fixtures together,
  inserts representative rows, and validates indexed reads before and after
  reopen.
- Run the focused storage-engine smoke test.
- Run `tools/mylite-compat-harness report application-schema`.
- Run format, tidy, diff, dev, embedded, and storage-smoke checks before
  commit.

## Acceptance Criteria

- The WordPress 6.9.4 multisite global and blog-id-2 schema fixtures import
  together without durable MariaDB sidecars.
- All 18 imported tables are cataloged as requested `DEFAULT`, effective
  `MYLITE`.
- Representative `wp_blogs`, `wp_2_options`, `wp_2_posts`, `wp_2_postmeta`,
  taxonomy, comments, commentmeta, and links reads pass before and after
  reopen.
- Roadmap and compatibility docs describe multisite blog-schema coverage
  without claiming full multisite runtime support.

## Risks And Open Questions

- Static DDL and seed rows do not prove WordPress multisite PHP runtime,
  network upgrade behavior, or per-site migration behavior.
- Additional blog ids and main-site prefix behavior remain separate
  compatibility cases.
