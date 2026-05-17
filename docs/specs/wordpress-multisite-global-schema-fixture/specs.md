# WordPress Multisite Global Schema Fixture

## Problem

MyLite has WordPress-shaped application-schema coverage and a pinned
single-site installer schema/seed fixture, but it does not yet cover WordPress
multisite global tables. Multisite changes the users table shape and adds
network-level tables with composite prefix indexes that are important
application-schema pressure for MyLite routing and catalog durability.

This slice adds a bounded multisite global schema fixture. It proves MyLite can
import the WordPress multisite global DDL through omitted-engine routing without
claiming full WordPress multisite runtime support.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- WordPress `wp-admin/includes/schema.php:36-53` defines
  `wp_get_db_schema()`, derives `$charset_collate`, detects multisite through
  `is_multisite()` or `WP_INSTALLING_NETWORK`, and fixes
  `$max_index_length = 191`.
- `schema.php:209-245` defines the multisite users table with `spam` and
  `deleted` columns and combines it with `wp_usermeta` for multisite global
  tables.
- `schema.php:248-316` defines multisite-only global tables:
  `wp_blogs`, `wp_blogmeta`, `wp_registration_log`, `wp_site`,
  `wp_sitemeta`, and `wp_signups`.
- `schema.php:318-337` returns multisite globals for the `ms_global` scope and
  includes them in `global`/`all` only when multisite is active.
- The DDL has no `ENGINE` clauses. In the MyLite storage-smoke profile,
  omitted engines route to MyLite and are cataloged as requested `DEFAULT`,
  effective `MYLITE`.

Reference URLs:

- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/schema.php>
- <https://wordpress.org/documentation/article/wordpress-versions/>

## Design

Add `wordpress-6.9.4-multisite-global-schema.sql` beside the existing
WordPress fixtures. The fixture is deterministic SQL derived from
`wp_get_db_schema()` multisite global output:

- `$wpdb` table names use the default `wp_` prefix;
- `$max_index_length` is substituted with `191`;
- `$charset_collate` is substituted with
  `DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci`;
- no `ENGINE` clauses are added.

The storage-smoke test creates a WordPress-compatible schema, imports the
fixture through `mylite_exec()`, checks all table catalog metadata, inserts a
representative network/site/blog/user/signup row set, exercises indexed reads,
closes and reopens the `.mylite` file, and repeats metadata and row assertions.

## Supported Scope

- WordPress 6.9.4 multisite global schema DDL.
- Multisite `wp_users` with `spam` and `deleted`.
- `wp_usermeta`, `wp_blogs`, `wp_blogmeta`, `wp_registration_log`, `wp_site`,
  `wp_sitemeta`, and `wp_signups`.
- Omitted-engine routing to MyLite.
- `utf8mb4_unicode_ci` table defaults.
- Catalog metadata, row/index visibility, and close/reopen discovery.

## Non-Goals

- Running WordPress PHP, WP-CLI, or network installation flow.
- Blog-specific multisite tables or per-site table prefixes.
- Multisite seed defaults, network options, blog creation hooks, rewrite
  behavior, upload paths, cron, mail, or upgrade paths.
- Plugin tables or broader ORM suites.
- Foreign-key enforcement; WordPress multisite tables do not declare database
  foreign keys.

## Compatibility Impact

Application-schema compatibility remains partial, but MyLite gains committed
coverage for the WordPress multisite global table DDL and representative
indexed row paths. The compatibility matrix should still distinguish this from
full WordPress multisite runtime support.

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
- Add storage-smoke coverage that imports it, inserts representative rows, and
  validates indexed reads before and after reopen.
- Run the focused storage-engine smoke test.
- Run `tools/mylite-compat-harness report application-schema`.
- Run format, tidy, diff, dev, embedded, and storage-smoke checks before
  commit.

## Acceptance Criteria

- The WordPress 6.9.4 multisite global schema fixture imports without durable
  MariaDB sidecars.
- All imported tables are cataloged as requested `DEFAULT`, effective
  `MYLITE`.
- Representative `wp_site`, `wp_blogs`, `wp_sitemeta`, `wp_users`, and
  `wp_signups` indexed reads pass before and after reopen.
- Roadmap and compatibility docs describe multisite global schema coverage
  without claiming full multisite runtime support.

## Risks And Open Questions

- Static DDL fixture coverage does not prove WordPress multisite PHP runtime
  behavior or upgrade behavior.
- Per-site blog tables and seeded network option rows remain separate
  application-schema slices.
