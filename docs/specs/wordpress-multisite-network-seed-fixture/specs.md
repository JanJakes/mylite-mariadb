# WordPress Multisite Network Seed Fixture

## Problem

The multisite global and blog schema fixtures prove DDL import, but the
representative seed rows still live inline in the C storage-smoke test. MyLite
needs a pinned SQL fixture for multisite seed data so application-schema
coverage tracks WordPress network population behavior in the same way the
single-site installer seed fixture does.

This slice adds a deterministic multisite network/blog seed fixture and proves
the seeded network metadata plus blog rows survive close/reopen.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

WordPress source: `WordPress/WordPress` tag `6.9.4`
(`97b7f62adb5d8864c3fac554bc7182d9fd754a41`).

- WordPress `wp-admin/includes/schema.php:977-983` `install_network()` creates
  global multisite tables through `wp_get_db_schema('global')`.
- `schema.php:1005-1081` `populate_network()` inserts the network row into
  `wp_site` and delegates network option rows to `populate_network_meta()`.
- `schema.php:1095-1133` inserts the main blog row, updates the network admin's
  `source_domain` and `primary_blog` usermeta rows, and inserts `main_site`
  network metadata when converting a single site to multisite.
- `schema.php:1231-1372` `populate_network_meta()` assembles default
  `wp_sitemeta` rows, serializes array-valued metadata such as `site_admins`
  and `active_sitewide_plugins`, and bulk inserts them.
- `schema.php:1385-1419` `populate_site_meta()` serializes and inserts
  per-site metadata into `wp_blogmeta`.
- The single-site installer seed fixture already covers representative
  `wp_install_defaults()` blog content. This slice uses a multisite-shaped
  deterministic subset for blog id `2` rather than rerunning PHP.

Reference URLs:

- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/schema.php>
- <https://github.com/WordPress/WordPress/blob/6.9.4/wp-admin/includes/upgrade.php>
- <https://wordpress.org/documentation/article/wordpress-versions/>

## Design

Add `wordpress-6.9.4-multisite-network-seed.sql` beside the existing WordPress
fixtures. The fixture is deterministic SQL:

- network domain is `example.test`;
- the network has a main blog id `1` at `/` and a representative blog id `2`
  at `/second/`;
- network administrator is `network-admin`;
- timestamps are fixed;
- array-valued network metadata is pre-serialized;
- no WordPress PHP, WP-CLI, cron, mail, rewrite, or cache behavior is run.

Update the combined multisite storage-smoke test to import:

1. `wordpress-6.9.4-multisite-global-schema.sql`;
2. `wordpress-6.9.4-multisite-blog-2-schema.sql`;
3. `wordpress-6.9.4-multisite-network-seed.sql`.

The test then asserts catalog metadata, representative network metadata,
administrator usermeta, and indexed blog-table rows before and after reopening
the `.mylite` file.

## Supported Scope

- Representative WordPress 6.9.4 multisite network seed rows.
- `wp_site`, `wp_sitemeta`, `wp_blogs`, `wp_blogmeta`, `wp_users`,
  `wp_usermeta`, and representative `wp_2_*` blog content rows.
- Serialized network metadata values containing semicolons inside SQL string
  literals.
- Close/reopen discovery and indexed reads over seeded rows.

## Non-Goals

- Running WordPress PHP, WP-CLI, or the network installer.
- Exhaustively reproducing every `populate_network_meta()` default.
- Main-site conversion edge cases, subdomain install behavior, cron cleanup,
  rewrite flushing, uploads directories, mail, cache invalidation, or upgrade
  paths.
- Multiple blog ids or plugin tables.

## Compatibility Impact

Application-schema compatibility remains partial, but the WordPress multisite
coverage now includes deterministic schema and seed fixtures. The compatibility
matrix should still distinguish this from full WordPress multisite runtime
support.

## Single-File And Embedded-Lifecycle Impact

The seed fixture must keep durable state in the primary `.mylite` file. The
storage-smoke sidecar gates continue to reject durable MariaDB sidecars after
close and after reopen.

## Public API Or File-Format Impact

No public API or file-format change is intended. The fixture uses
`mylite_exec()` over existing table, index, row, and catalog behavior.

## Storage-Engine Routing Impact

No new engine mapping is added. The seed runs against tables imported by the
omitted-engine WordPress multisite schema fixtures.

## Binary-Size Impact

No default build-profile or linked-runtime change is expected. This slice adds
test fixture data only.

## License Or Dependency Impact

The fixture is derived from GPL-2.0-or-later WordPress network population
behavior and is compatible with this GPL-2.0 MariaDB-derived repository. No
runtime dependency is added.

## Test And Verification Plan

- Add the seed fixture.
- Update `mylite_embedded_storage_engine_test` to import the seed after the
  multisite global and blog schema fixtures.
- Assert representative network metadata, usermeta, and blog rows before and
  after reopen.
- Run the application-schema compatibility group.
- Run format, tidy, diff, dev, embedded, and storage-smoke checks before commit.

## Acceptance Criteria

- The multisite schema and seed fixtures import together in one storage-smoke
  database.
- Serialized network metadata values do not break fixture execution.
- Representative network metadata, usermeta, blog identity, options, posts,
  taxonomy, comments, commentmeta, and link rows read back before and after
  reopen.
- No durable sidecars remain after close.
- Compatibility and roadmap docs distinguish this deterministic seed fixture
  from full WordPress multisite runtime support.

## Risks And Open Questions

- A static SQL seed cannot cover WordPress filters, generated text, theme
  discovery, upload MIME enumeration, cron behavior, rewrite flushing, or mail
  paths.
- The seed is representative rather than exhaustive. A later runtime installer
  test should compare against an actual WordPress multisite install database.
